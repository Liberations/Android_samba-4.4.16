/*
 * Store streams in a separate subdirectory
 *
 * Copyright (C) Volker Lendecke, 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"
#include "smbd/smbd.h"
#include "system/filesys.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

/*
 * Excerpt from a mail from tridge:
 *
 * Volker, what I'm thinking of is this:
 * /mount-point/.streams/XX/YY/aaaa.bbbb/namedstream1
 * /mount-point/.streams/XX/YY/aaaa.bbbb/namedstream2
 *
 * where XX/YY is a 2 level hash based on the fsid/inode. "aaaa.bbbb"
 * is the fsid/inode. "namedstreamX" is a file named after the stream
 * name.
 */

static uint32_t hash_fn(DATA_BLOB key)
{
	uint32_t value;	/* Used to compute the hash value.  */
	uint32_t i;	/* Used to cycle through random values. */

	/* Set the initial value from the key size. */
	for (value = 0x238F13AF * key.length, i=0; i < key.length; i++)
		value = (value + (key.data[i] << (i*5 % 24)));

	return (1103515243 * value + 12345);
}

/*
 * With the hashing scheme based on the inode we need to protect against
 * streams showing up on files with re-used inodes. This can happen if we
 * create a stream directory from within Samba, and a local process or NFS
 * client deletes the file without deleting the streams directory. When the
 * inode is re-used and the stream directory is still around, the streams in
 * there would be show up as belonging to the new file.
 *
 * There are several workarounds for this, probably the easiest one is on
 * systems which have a true birthtime stat element: When the file has a later
 * birthtime than the streams directory, then we have to recreate the
 * directory.
 *
 * The other workaround is to somehow mark the file as generated by Samba with
 * something that a NFS client would not do. The closest one is a special
 * xattr value being set. On systems which do not support xattrs, it might be
 * an option to put in a special ACL entry for a non-existing group.
 */

static bool file_is_valid(vfs_handle_struct *handle, const char *path)
{
	char buf;

	DEBUG(10, ("file_is_valid (%s) called\n", path));

	if (SMB_VFS_GETXATTR(handle->conn, path, SAMBA_XATTR_MARKER,
				  &buf, sizeof(buf)) != sizeof(buf)) {
		DEBUG(10, ("GETXATTR failed: %s\n", strerror(errno)));
		return false;
	}

	if (buf != '1') {
		DEBUG(10, ("got wrong buffer content: '%c'\n", buf));
		return false;
	}

	return true;
}

static bool mark_file_valid(vfs_handle_struct *handle, const char *path)
{
	char buf = '1';
	int ret;

	DEBUG(10, ("marking file %s as valid\n", path));

	ret = SMB_VFS_SETXATTR(handle->conn, path, SAMBA_XATTR_MARKER,
				    &buf, sizeof(buf), 0);

	if (ret == -1) {
		DEBUG(10, ("SETXATTR failed: %s\n", strerror(errno)));
		return false;
	}

	return true;
}

/**
 * Given an smb_filename, determine the stream directory using the file's
 * base_name.
 */
static char *stream_dir(vfs_handle_struct *handle,
			const struct smb_filename *smb_fname,
			const SMB_STRUCT_STAT *base_sbuf, bool create_it)
{
	uint32_t hash;
	struct smb_filename *smb_fname_hash = NULL;
	char *result = NULL;
	SMB_STRUCT_STAT base_sbuf_tmp;
	uint8_t first, second;
	char *tmp;
	char *id_hex;
	struct file_id id;
	uint8_t id_buf[16];
	bool check_valid;
	const char *rootdir;

	check_valid = lp_parm_bool(SNUM(handle->conn),
		      "streams_depot", "check_valid", true);

	tmp = talloc_asprintf(talloc_tos(), "%s/.streams",
		handle->conn->connectpath);

	if (tmp == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	rootdir = lp_parm_const_string(
		SNUM(handle->conn), "streams_depot", "directory",
		tmp);

	/* Stat the base file if it hasn't already been done. */
	if (base_sbuf == NULL) {
		struct smb_filename *smb_fname_base;

		smb_fname_base = synthetic_smb_fname(
			talloc_tos(), smb_fname->base_name, NULL, NULL);
		if (smb_fname_base == NULL) {
			errno = ENOMEM;
			goto fail;
		}
		if (SMB_VFS_NEXT_STAT(handle, smb_fname_base) == -1) {
			TALLOC_FREE(smb_fname_base);
			goto fail;
		}
		base_sbuf_tmp = smb_fname_base->st;
		TALLOC_FREE(smb_fname_base);
	} else {
		base_sbuf_tmp = *base_sbuf;
	}

	id = SMB_VFS_FILE_ID_CREATE(handle->conn, &base_sbuf_tmp);

	push_file_id_16((char *)id_buf, &id);

	hash = hash_fn(data_blob_const(id_buf, sizeof(id_buf)));

	first = hash & 0xff;
	second = (hash >> 8) & 0xff;

	id_hex = hex_encode_talloc(talloc_tos(), id_buf, sizeof(id_buf));

	if (id_hex == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	result = talloc_asprintf(talloc_tos(), "%s/%2.2X/%2.2X/%s", rootdir,
				 first, second, id_hex);

	TALLOC_FREE(id_hex);

	if (result == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	smb_fname_hash = synthetic_smb_fname(talloc_tos(), result, NULL, NULL);
	if (smb_fname_hash == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	if (SMB_VFS_NEXT_STAT(handle, smb_fname_hash) == 0) {
		struct smb_filename *smb_fname_new = NULL;
		char *newname;
		bool delete_lost;

		if (!S_ISDIR(smb_fname_hash->st.st_ex_mode)) {
			errno = EINVAL;
			goto fail;
		}

		if (!check_valid ||
		    file_is_valid(handle, smb_fname->base_name)) {
			return result;
		}

		/*
		 * Someone has recreated a file under an existing inode
		 * without deleting the streams directory.
		 * Move it away or remove if streams_depot:delete_lost is set.
		 */

	again:
		delete_lost = lp_parm_bool(SNUM(handle->conn), "streams_depot",
					   "delete_lost", false);

		if (delete_lost) {
			DEBUG(3, ("Someone has recreated a file under an "
			      "existing inode. Removing: %s\n",
			      smb_fname_hash->base_name));
			recursive_rmdir(talloc_tos(), handle->conn,
					smb_fname_hash);
			SMB_VFS_NEXT_RMDIR(handle, smb_fname_hash->base_name);
		} else {
			newname = talloc_asprintf(talloc_tos(), "lost-%lu",
						  random());
			DEBUG(3, ("Someone has recreated a file under an "
			      "existing inode. Renaming: %s to: %s\n",
			      smb_fname_hash->base_name,
			      newname));
			if (newname == NULL) {
				errno = ENOMEM;
				goto fail;
			}

			smb_fname_new = synthetic_smb_fname(
				talloc_tos(), newname, NULL, NULL);
			TALLOC_FREE(newname);
			if (smb_fname_new == NULL) {
				errno = ENOMEM;
				goto fail;
			}

			if (SMB_VFS_NEXT_RENAME(handle, smb_fname_hash,
						smb_fname_new) == -1) {
				TALLOC_FREE(smb_fname_new);
				if ((errno == EEXIST) || (errno == ENOTEMPTY)) {
					goto again;
				}
				goto fail;
			}

			TALLOC_FREE(smb_fname_new);
		}
	}

	if (!create_it) {
		errno = ENOENT;
		goto fail;
	}

	if ((SMB_VFS_NEXT_MKDIR(handle, rootdir, 0755) != 0)
	    && (errno != EEXIST)) {
		goto fail;
	}

	tmp = talloc_asprintf(result, "%s/%2.2X", rootdir, first);
	if (tmp == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	if ((SMB_VFS_NEXT_MKDIR(handle, tmp, 0755) != 0)
	    && (errno != EEXIST)) {
		goto fail;
	}

	TALLOC_FREE(tmp);

	tmp = talloc_asprintf(result, "%s/%2.2X/%2.2X", rootdir, first,
			      second);
	if (tmp == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	if ((SMB_VFS_NEXT_MKDIR(handle, tmp, 0755) != 0)
	    && (errno != EEXIST)) {
		goto fail;
	}

	TALLOC_FREE(tmp);

	if ((SMB_VFS_NEXT_MKDIR(handle, result, 0755) != 0)
	    && (errno != EEXIST)) {
		goto fail;
	}

	if (check_valid && !mark_file_valid(handle, smb_fname->base_name)) {
		goto fail;
	}

	TALLOC_FREE(smb_fname_hash);
	return result;

 fail:
	TALLOC_FREE(smb_fname_hash);
	TALLOC_FREE(result);
	return NULL;
}
/**
 * Given a stream name, populate smb_fname_out with the actual location of the
 * stream.
 */
static NTSTATUS stream_smb_fname(vfs_handle_struct *handle,
				 const struct smb_filename *smb_fname,
				 struct smb_filename **smb_fname_out,
				 bool create_dir)
{
	char *dirname, *stream_fname;
	const char *stype;
	NTSTATUS status;

	*smb_fname_out = NULL;

	stype = strchr_m(smb_fname->stream_name + 1, ':');

	if (stype) {
		if (strcasecmp_m(stype, ":$DATA") != 0) {
			return NT_STATUS_INVALID_PARAMETER;
		}
	}

	dirname = stream_dir(handle, smb_fname, NULL, create_dir);

	if (dirname == NULL) {
		status = map_nt_error_from_unix(errno);
		goto fail;
	}

	stream_fname = talloc_asprintf(talloc_tos(), "%s/%s", dirname,
				       smb_fname->stream_name);

	if (stream_fname == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (stype == NULL) {
		/* Append an explicit stream type if one wasn't specified. */
		stream_fname = talloc_asprintf(talloc_tos(), "%s:$DATA",
					       stream_fname);
		if (stream_fname == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto fail;
		}
	} else {
		/* Normalize the stream type to upercase. */
		if (!strupper_m(strrchr_m(stream_fname, ':') + 1)) {
			status = NT_STATUS_INVALID_PARAMETER;
			goto fail;
		}
	}

	DEBUG(10, ("stream filename = %s\n", stream_fname));

	/* Create an smb_filename with stream_name == NULL. */
	*smb_fname_out = synthetic_smb_fname(
		talloc_tos(), stream_fname, NULL, NULL);
	if (*smb_fname_out == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;

 fail:
	DEBUG(5, ("stream_name failed: %s\n", strerror(errno)));
	TALLOC_FREE(*smb_fname_out);
	return status;
}

static NTSTATUS walk_streams(vfs_handle_struct *handle,
			     struct smb_filename *smb_fname_base,
			     char **pdirname,
			     bool (*fn)(const char *dirname,
					const char *dirent,
					void *private_data),
			     void *private_data)
{
	char *dirname;
	DIR *dirhandle = NULL;
	const char *dirent = NULL;
	char *talloced = NULL;

	dirname = stream_dir(handle, smb_fname_base, &smb_fname_base->st,
			     false);

	if (dirname == NULL) {
		if (errno == ENOENT) {
			/*
			 * no stream around
			 */
			return NT_STATUS_OK;
		}
		return map_nt_error_from_unix(errno);
	}

	DEBUG(10, ("walk_streams: dirname=%s\n", dirname));

	dirhandle = SMB_VFS_NEXT_OPENDIR(handle, dirname, NULL, 0);

	if (dirhandle == NULL) {
		TALLOC_FREE(dirname);
		return map_nt_error_from_unix(errno);
	}

	while ((dirent = vfs_readdirname(handle->conn, dirhandle, NULL,
					 &talloced)) != NULL) {

		if (ISDOT(dirent) || ISDOTDOT(dirent)) {
			TALLOC_FREE(talloced);
			continue;
		}

		DEBUG(10, ("walk_streams: dirent=%s\n", dirent));

		if (!fn(dirname, dirent, private_data)) {
			TALLOC_FREE(talloced);
			break;
		}
		TALLOC_FREE(talloced);
	}

	SMB_VFS_NEXT_CLOSEDIR(handle, dirhandle);

	if (pdirname != NULL) {
		*pdirname = dirname;
	}
	else {
		TALLOC_FREE(dirname);
	}

	return NT_STATUS_OK;
}

/**
 * Helper to stat/lstat the base file of an smb_fname. This will actually
 * fills in the stat struct in smb_filename.
 */
static int streams_depot_stat_base(vfs_handle_struct *handle,
				   struct smb_filename *smb_fname,
				   bool follow_links)
{
	char *tmp_stream_name;
	int result;

	tmp_stream_name = smb_fname->stream_name;
	smb_fname->stream_name = NULL;
	if (follow_links) {
		result = SMB_VFS_NEXT_STAT(handle, smb_fname);
	} else {
		result = SMB_VFS_NEXT_LSTAT(handle, smb_fname);
	}
	smb_fname->stream_name = tmp_stream_name;
	return result;
}

static int streams_depot_stat(vfs_handle_struct *handle,
			      struct smb_filename *smb_fname)
{
	struct smb_filename *smb_fname_stream = NULL;
	NTSTATUS status;
	int ret = -1;

	DEBUG(10, ("streams_depot_stat called for [%s]\n",
		   smb_fname_str_dbg(smb_fname)));

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_STAT(handle, smb_fname);
	}

	/* If the default stream is requested, just stat the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		return streams_depot_stat_base(handle, smb_fname, true);
	}

	/* Stat the actual stream now. */
	status = stream_smb_fname(handle, smb_fname, &smb_fname_stream,
				  false);
	if (!NT_STATUS_IS_OK(status)) {
		ret = -1;
		errno = map_errno_from_nt_status(status);
		goto done;
	}

	ret = SMB_VFS_NEXT_STAT(handle, smb_fname_stream);

	/* Update the original smb_fname with the stat info. */
	smb_fname->st = smb_fname_stream->st;
 done:
	TALLOC_FREE(smb_fname_stream);
	return ret;
}



static int streams_depot_lstat(vfs_handle_struct *handle,
			       struct smb_filename *smb_fname)
{
	struct smb_filename *smb_fname_stream = NULL;
	NTSTATUS status;
	int ret = -1;

	DEBUG(10, ("streams_depot_lstat called for [%s]\n",
		   smb_fname_str_dbg(smb_fname)));

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_LSTAT(handle, smb_fname);
	}

	/* If the default stream is requested, just stat the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		return streams_depot_stat_base(handle, smb_fname, false);
	}

	/* Stat the actual stream now. */
	status = stream_smb_fname(handle, smb_fname, &smb_fname_stream,
				  false);
	if (!NT_STATUS_IS_OK(status)) {
		ret = -1;
		errno = map_errno_from_nt_status(status);
		goto done;
	}

	ret = SMB_VFS_NEXT_LSTAT(handle, smb_fname_stream);

 done:
	TALLOC_FREE(smb_fname_stream);
	return ret;
}

static int streams_depot_open(vfs_handle_struct *handle,
			      struct smb_filename *smb_fname,
			      files_struct *fsp, int flags, mode_t mode)
{
	struct smb_filename *smb_fname_stream = NULL;
	struct smb_filename *smb_fname_base = NULL;
	NTSTATUS status;
	int ret = -1;

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_OPEN(handle, smb_fname, fsp, flags, mode);
	}

	/* If the default stream is requested, just open the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		char *tmp_stream_name;

		tmp_stream_name = smb_fname->stream_name;
		smb_fname->stream_name = NULL;
		ret = SMB_VFS_NEXT_OPEN(handle, smb_fname, fsp, flags, mode);
		smb_fname->stream_name = tmp_stream_name;

		return ret;
	}

	/* Ensure the base file still exists. */
	smb_fname_base = synthetic_smb_fname(
		talloc_tos(), smb_fname->base_name, NULL, NULL);
	if (smb_fname_base == NULL) {
		ret = -1;
		errno = ENOMEM;
		goto done;
	}

	ret = SMB_VFS_NEXT_STAT(handle, smb_fname_base);
	if (ret == -1) {
		goto done;
	}

	/* Determine the stream name, and then open it. */
	status = stream_smb_fname(handle, smb_fname, &smb_fname_stream, true);
	if (!NT_STATUS_IS_OK(status)) {
		ret = -1;
		errno = map_errno_from_nt_status(status);
		goto done;
	}

	ret = SMB_VFS_NEXT_OPEN(handle, smb_fname_stream, fsp, flags, mode);

 done:
	TALLOC_FREE(smb_fname_stream);
	TALLOC_FREE(smb_fname_base);
	return ret;
}

static int streams_depot_unlink(vfs_handle_struct *handle,
				const struct smb_filename *smb_fname)
{
	struct smb_filename *smb_fname_base = NULL;
	int ret = -1;

	DEBUG(10, ("streams_depot_unlink called for %s\n",
		   smb_fname_str_dbg(smb_fname)));

	/* If there is a valid stream, just unlink the stream and return. */
	if (is_ntfs_stream_smb_fname(smb_fname) &&
	    !is_ntfs_default_stream_smb_fname(smb_fname)) {
		struct smb_filename *smb_fname_stream = NULL;
		NTSTATUS status;

		status = stream_smb_fname(handle, smb_fname, &smb_fname_stream,
					  false);
		if (!NT_STATUS_IS_OK(status)) {
			errno = map_errno_from_nt_status(status);
			return -1;
		}

		ret = SMB_VFS_NEXT_UNLINK(handle, smb_fname_stream);

		TALLOC_FREE(smb_fname_stream);
		return ret;
	}

	/*
	 * We potentially need to delete the per-inode streams directory
	 */

	smb_fname_base = synthetic_smb_fname(
		talloc_tos(), smb_fname->base_name, NULL, NULL);
	if (smb_fname_base == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if (lp_posix_pathnames()) {
		ret = SMB_VFS_NEXT_LSTAT(handle, smb_fname_base);
	} else {
		ret = SMB_VFS_NEXT_STAT(handle, smb_fname_base);
	}

	if (ret == -1) {
		TALLOC_FREE(smb_fname_base);
		return -1;
	}

	ret = SMB_VFS_NEXT_UNLINK(handle, smb_fname);
	if (ret == 0) {
		char *dirname = stream_dir(handle, smb_fname_base,
					   &smb_fname_base->st, false);

		if (dirname != NULL) {
			SMB_VFS_NEXT_RMDIR(handle, dirname);
		}
		TALLOC_FREE(dirname);
	}

	TALLOC_FREE(smb_fname_base);
	return ret;
}

static int streams_depot_rmdir(vfs_handle_struct *handle, const char *path)
{
	struct smb_filename *smb_fname_base = NULL;
	int ret = -1;

	DEBUG(10, ("streams_depot_rmdir called for %s\n", path));

	/*
	 * We potentially need to delete the per-inode streams directory
	 */

	smb_fname_base = synthetic_smb_fname(talloc_tos(), path, NULL, NULL);
	if (smb_fname_base == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if (lp_posix_pathnames()) {
		ret = SMB_VFS_NEXT_LSTAT(handle, smb_fname_base);
	} else {
		ret = SMB_VFS_NEXT_STAT(handle, smb_fname_base);
	}

	if (ret == -1) {
		TALLOC_FREE(smb_fname_base);
		return -1;
	}

	ret = SMB_VFS_NEXT_RMDIR(handle, path);
	if (ret == 0) {
		char *dirname = stream_dir(handle, smb_fname_base,
					   &smb_fname_base->st, false);

		if (dirname != NULL) {
			SMB_VFS_NEXT_RMDIR(handle, dirname);
		}
		TALLOC_FREE(dirname);
	}

	TALLOC_FREE(smb_fname_base);
	return ret;
}

static int streams_depot_rename(vfs_handle_struct *handle,
				const struct smb_filename *smb_fname_src,
				const struct smb_filename *smb_fname_dst)
{
	struct smb_filename *smb_fname_src_stream = NULL;
	struct smb_filename *smb_fname_dst_stream = NULL;
	bool src_is_stream, dst_is_stream;
	NTSTATUS status;
	int ret = -1;

	DEBUG(10, ("streams_depot_rename called for %s => %s\n",
		   smb_fname_str_dbg(smb_fname_src),
		   smb_fname_str_dbg(smb_fname_dst)));

	src_is_stream = is_ntfs_stream_smb_fname(smb_fname_src);
	dst_is_stream = is_ntfs_stream_smb_fname(smb_fname_dst);

	if (!src_is_stream && !dst_is_stream) {
		return SMB_VFS_NEXT_RENAME(handle, smb_fname_src,
					   smb_fname_dst);
	}

	/* for now don't allow renames from or to the default stream */
	if (is_ntfs_default_stream_smb_fname(smb_fname_src) ||
	    is_ntfs_default_stream_smb_fname(smb_fname_dst)) {
		errno = ENOSYS;
		goto done;
	}

	status = stream_smb_fname(handle, smb_fname_src, &smb_fname_src_stream,
				  false);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto done;
	}

	status = stream_smb_fname(handle, smb_fname_dst,
				  &smb_fname_dst_stream, false);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto done;
	}

	ret = SMB_VFS_NEXT_RENAME(handle, smb_fname_src_stream,
				  smb_fname_dst_stream);

done:
	TALLOC_FREE(smb_fname_src_stream);
	TALLOC_FREE(smb_fname_dst_stream);
	return ret;
}

static bool add_one_stream(TALLOC_CTX *mem_ctx, unsigned int *num_streams,
			   struct stream_struct **streams,
			   const char *name, off_t size,
			   off_t alloc_size)
{
	struct stream_struct *tmp;

	tmp = talloc_realloc(mem_ctx, *streams, struct stream_struct,
				   (*num_streams)+1);
	if (tmp == NULL) {
		return false;
	}

	tmp[*num_streams].name = talloc_strdup(tmp, name);
	if (tmp[*num_streams].name == NULL) {
		return false;
	}

	tmp[*num_streams].size = size;
	tmp[*num_streams].alloc_size = alloc_size;

	*streams = tmp;
	*num_streams += 1;
	return true;
}

struct streaminfo_state {
	TALLOC_CTX *mem_ctx;
	vfs_handle_struct *handle;
	unsigned int num_streams;
	struct stream_struct *streams;
	NTSTATUS status;
};

static bool collect_one_stream(const char *dirname,
			       const char *dirent,
			       void *private_data)
{
	struct streaminfo_state *state =
		(struct streaminfo_state *)private_data;
	struct smb_filename *smb_fname = NULL;
	char *sname = NULL;
	bool ret;

	sname = talloc_asprintf(talloc_tos(), "%s/%s", dirname, dirent);
	if (sname == NULL) {
		state->status = NT_STATUS_NO_MEMORY;
		ret = false;
		goto out;
	}

	smb_fname = synthetic_smb_fname(talloc_tos(), sname, NULL, NULL);
	if (smb_fname == NULL) {
		state->status = NT_STATUS_NO_MEMORY;
		ret = false;
		goto out;
	}

	if (SMB_VFS_NEXT_STAT(state->handle, smb_fname) == -1) {
		DEBUG(10, ("Could not stat %s: %s\n", sname,
			   strerror(errno)));
		ret = true;
		goto out;
	}

	if (!add_one_stream(state->mem_ctx,
			    &state->num_streams, &state->streams,
			    dirent, smb_fname->st.st_ex_size,
			    SMB_VFS_GET_ALLOC_SIZE(state->handle->conn, NULL,
						   &smb_fname->st))) {
		state->status = NT_STATUS_NO_MEMORY;
		ret = false;
		goto out;
	}

	ret = true;
 out:
	TALLOC_FREE(sname);
	TALLOC_FREE(smb_fname);
	return ret;
}

static NTSTATUS streams_depot_streaminfo(vfs_handle_struct *handle,
					 struct files_struct *fsp,
					 const char *fname,
					 TALLOC_CTX *mem_ctx,
					 unsigned int *pnum_streams,
					 struct stream_struct **pstreams)
{
	struct smb_filename *smb_fname_base;
	int ret;
	NTSTATUS status;
	struct streaminfo_state state;

	smb_fname_base = synthetic_smb_fname(talloc_tos(), fname, NULL, NULL);
	if (smb_fname_base == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	if ((fsp != NULL) && (fsp->fh->fd != -1)) {
		ret = SMB_VFS_NEXT_FSTAT(handle, fsp, &smb_fname_base->st);
	}
	else {
		if (lp_posix_pathnames()) {
			ret = SMB_VFS_NEXT_LSTAT(handle, smb_fname_base);
		} else {
			ret = SMB_VFS_NEXT_STAT(handle, smb_fname_base);
		}
	}

	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		goto out;
	}

	state.streams = *pstreams;
	state.num_streams = *pnum_streams;
	state.mem_ctx = mem_ctx;
	state.handle = handle;
	state.status = NT_STATUS_OK;

	if (S_ISLNK(smb_fname_base->st.st_ex_mode)) {
		/*
		 * Currently we do't have SMB_VFS_LLISTXATTR
		 * inside the VFS which means there's no way
		 * to cope with a symlink when lp_posix_pathnames().
		 * returns true. For now ignore links.
		 * FIXME - by adding SMB_VFS_LLISTXATTR. JRA.
		 */
		status = NT_STATUS_OK;
	} else {
		status = walk_streams(handle, smb_fname_base, NULL, collect_one_stream,
			      &state);
	}

	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(state.streams);
		goto out;
	}

	if (!NT_STATUS_IS_OK(state.status)) {
		TALLOC_FREE(state.streams);
		status = state.status;
		goto out;
	}

	*pnum_streams = state.num_streams;
	*pstreams = state.streams;
	status = SMB_VFS_NEXT_STREAMINFO(handle, fsp, fname, mem_ctx, pnum_streams, pstreams);

 out:
	TALLOC_FREE(smb_fname_base);
	return status;
}

static uint32_t streams_depot_fs_capabilities(struct vfs_handle_struct *handle,
			enum timestamp_set_resolution *p_ts_res)
{
	return SMB_VFS_NEXT_FS_CAPABILITIES(handle, p_ts_res) | FILE_NAMED_STREAMS;
}

static struct vfs_fn_pointers vfs_streams_depot_fns = {
	.fs_capabilities_fn = streams_depot_fs_capabilities,
	.open_fn = streams_depot_open,
	.stat_fn = streams_depot_stat,
	.lstat_fn = streams_depot_lstat,
	.unlink_fn = streams_depot_unlink,
	.rmdir_fn = streams_depot_rmdir,
	.rename_fn = streams_depot_rename,
	.streaminfo_fn = streams_depot_streaminfo,
};

NTSTATUS vfs_streams_depot_init(void);
NTSTATUS vfs_streams_depot_init(void)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "streams_depot",
				&vfs_streams_depot_fns);
}
