/*
 * Store streams in xattrs
 *
 * Copyright (C) Volker Lendecke, 2008
 *
 * Partly based on James Peach's Darwin module, which is
 *
 * Copyright (C) James Peach 2006-2007
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
#include "../lib/crypto/md5.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

struct streams_xattr_config {
	const char *prefix;
	size_t prefix_len;
	bool store_stream_type;
};

struct stream_io {
	char *base;
	char *xattr_name;
	void *fsp_name_ptr;
	files_struct *fsp;
	vfs_handle_struct *handle;
};

static SMB_INO_T stream_inode(const SMB_STRUCT_STAT *sbuf, const char *sname)
{
	MD5_CTX ctx;
        unsigned char hash[16];
	SMB_INO_T result;
	char *upper_sname;

	DEBUG(10, ("stream_inode called for %lu/%lu [%s]\n",
		   (unsigned long)sbuf->st_ex_dev,
		   (unsigned long)sbuf->st_ex_ino, sname));

	upper_sname = talloc_strdup_upper(talloc_tos(), sname);
	SMB_ASSERT(upper_sname != NULL);

        MD5Init(&ctx);
        MD5Update(&ctx, (const unsigned char *)&(sbuf->st_ex_dev),
		  sizeof(sbuf->st_ex_dev));
        MD5Update(&ctx, (const unsigned char *)&(sbuf->st_ex_ino),
		  sizeof(sbuf->st_ex_ino));
        MD5Update(&ctx, (unsigned char *)upper_sname,
		  talloc_get_size(upper_sname)-1);
        MD5Final(hash, &ctx);

	TALLOC_FREE(upper_sname);

        /* Hopefully all the variation is in the lower 4 (or 8) bytes! */
	memcpy(&result, hash, sizeof(result));

	DEBUG(10, ("stream_inode returns %lu\n", (unsigned long)result));

	return result;
}

static ssize_t get_xattr_size(connection_struct *conn,
				files_struct *fsp,
				const char *fname,
				const char *xattr_name)
{
	NTSTATUS status;
	struct ea_struct ea;
	ssize_t result;

	status = get_ea_value(talloc_tos(), conn, fsp, fname,
			      xattr_name, &ea);

	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

	result = ea.value.length-1;
	TALLOC_FREE(ea.value.data);
	return result;
}

/**
 * Given a stream name, populate xattr_name with the xattr name to use for
 * accessing the stream.
 */
static NTSTATUS streams_xattr_get_name(vfs_handle_struct *handle,
				       TALLOC_CTX *ctx,
				       const char *stream_name,
				       char **xattr_name)
{
	char *sname;
	char *stype;
	struct streams_xattr_config *config;

	SMB_VFS_HANDLE_GET_DATA(handle, config, struct streams_xattr_config,
				return NT_STATUS_UNSUCCESSFUL);

	sname = talloc_strdup(ctx, stream_name + 1);
	if (sname == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * With vfs_fruit option "fruit:encoding = native" we're
	 * already converting stream names that contain illegal NTFS
	 * characters from their on-the-wire Unicode Private Range
	 * encoding to their native ASCII representation.
	 *
	 * As as result the name of xattrs storing the streams (via
	 * vfs_streams_xattr) may contain a colon, so we have to use
	 * strrchr_m() instead of strchr_m() for matching the stream
	 * type suffix.
	 *
	 * In check_path_syntax() we've already ensured the streamname
	 * we got from the client is valid.
	 */
	stype = strrchr_m(sname, ':');

	if (stype) {
		/*
		 * We only support one stream type: "$DATA"
		 */
		if (strcasecmp_m(stype, ":$DATA") != 0) {
			talloc_free(sname);
			return NT_STATUS_INVALID_PARAMETER;
		}

		/* Split name and type */
		stype[0] = '\0';
	}

	*xattr_name = talloc_asprintf(ctx, "%s%s%s",
				      config->prefix,
				      sname,
				      config->store_stream_type ? ":$DATA" : "");
	if (*xattr_name == NULL) {
		talloc_free(sname);
		return NT_STATUS_NO_MEMORY;
	}

	DEBUG(10, ("xattr_name: %s, stream_name: %s\n", *xattr_name,
		   stream_name));

	talloc_free(sname);
	return NT_STATUS_OK;
}

static bool streams_xattr_recheck(struct stream_io *sio)
{
	NTSTATUS status;
	char *xattr_name = NULL;

	if (sio->fsp->fsp_name == sio->fsp_name_ptr) {
		return true;
	}

	if (sio->fsp->fsp_name->stream_name == NULL) {
		/* how can this happen */
		errno = EINVAL;
		return false;
	}

	status = streams_xattr_get_name(sio->handle, talloc_tos(),
					sio->fsp->fsp_name->stream_name,
					&xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		return false;
	}

	TALLOC_FREE(sio->xattr_name);
	TALLOC_FREE(sio->base);
	sio->xattr_name = talloc_strdup(VFS_MEMCTX_FSP_EXTENSION(sio->handle, sio->fsp),
					xattr_name);
	sio->base = talloc_strdup(VFS_MEMCTX_FSP_EXTENSION(sio->handle, sio->fsp),
				  sio->fsp->fsp_name->base_name);
	sio->fsp_name_ptr = sio->fsp->fsp_name;

	TALLOC_FREE(xattr_name);

	if ((sio->xattr_name == NULL) || (sio->base == NULL)) {
		return false;
	}

	return true;
}

/**
 * Helper to stat/lstat the base file of an smb_fname.
 */
static int streams_xattr_stat_base(vfs_handle_struct *handle,
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

static int streams_xattr_fstat(vfs_handle_struct *handle, files_struct *fsp,
			       SMB_STRUCT_STAT *sbuf)
{
	struct smb_filename *smb_fname_base = NULL;
	int ret = -1;
	struct stream_io *io = (struct stream_io *)
		VFS_FETCH_FSP_EXTENSION(handle, fsp);

	DEBUG(10, ("streams_xattr_fstat called for %d\n", fsp->fh->fd));

	if (io == NULL || fsp->base_fsp == NULL) {
		return SMB_VFS_NEXT_FSTAT(handle, fsp, sbuf);
	}

	if (!streams_xattr_recheck(io)) {
		return -1;
	}

	/* Create an smb_filename with stream_name == NULL. */
	smb_fname_base = synthetic_smb_fname(talloc_tos(), io->base,
					     NULL, NULL);
	if (smb_fname_base == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if (lp_posix_pathnames()) {
		ret = SMB_VFS_LSTAT(handle->conn, smb_fname_base);
	} else {
		ret = SMB_VFS_STAT(handle->conn, smb_fname_base);
	}
	*sbuf = smb_fname_base->st;
	TALLOC_FREE(smb_fname_base);

	if (ret == -1) {
		return -1;
	}

	sbuf->st_ex_size = get_xattr_size(handle->conn, fsp,
					io->base, io->xattr_name);
	if (sbuf->st_ex_size == -1) {
		return -1;
	}

	DEBUG(10, ("sbuf->st_ex_size = %d\n", (int)sbuf->st_ex_size));

	sbuf->st_ex_ino = stream_inode(sbuf, io->xattr_name);
	sbuf->st_ex_mode &= ~S_IFMT;
        sbuf->st_ex_mode |= S_IFREG;
        sbuf->st_ex_blocks = sbuf->st_ex_size / STAT_ST_BLOCKSIZE + 1;

	return 0;
}

static int streams_xattr_stat(vfs_handle_struct *handle,
			      struct smb_filename *smb_fname)
{
	NTSTATUS status;
	int result = -1;
	char *xattr_name = NULL;

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_STAT(handle, smb_fname);
	}

	/* Note if lp_posix_paths() is true, we can never
	 * get here as is_ntfs_stream_smb_fname() is
	 * always false. So we never need worry about
	 * not following links here. */

	/* If the default stream is requested, just stat the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		return streams_xattr_stat_base(handle, smb_fname, true);
	}

	/* Populate the stat struct with info from the base file. */
	if (streams_xattr_stat_base(handle, smb_fname, true) == -1) {
		return -1;
	}

	/* Derive the xattr name to lookup. */
	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname->stream_name, &xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		return -1;
	}

	/* Augment the base file's stat information before returning. */
	smb_fname->st.st_ex_size = get_xattr_size(handle->conn, NULL,
						  smb_fname->base_name,
						  xattr_name);
	if (smb_fname->st.st_ex_size == -1) {
		errno = ENOENT;
		result = -1;
		goto fail;
	}

	smb_fname->st.st_ex_ino = stream_inode(&smb_fname->st, xattr_name);
	smb_fname->st.st_ex_mode &= ~S_IFMT;
        smb_fname->st.st_ex_mode |= S_IFREG;
        smb_fname->st.st_ex_blocks =
	    smb_fname->st.st_ex_size / STAT_ST_BLOCKSIZE + 1;

	result = 0;
 fail:
	TALLOC_FREE(xattr_name);
	return result;
}

static int streams_xattr_lstat(vfs_handle_struct *handle,
			       struct smb_filename *smb_fname)
{
	NTSTATUS status;
	int result = -1;
	char *xattr_name = NULL;

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_LSTAT(handle, smb_fname);
	}

	/* If the default stream is requested, just stat the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		return streams_xattr_stat_base(handle, smb_fname, false);
	}

	/* Populate the stat struct with info from the base file. */
	if (streams_xattr_stat_base(handle, smb_fname, false) == -1) {
		return -1;
	}

	/* Derive the xattr name to lookup. */
	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname->stream_name, &xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		return -1;
	}

	/* Augment the base file's stat information before returning. */
	smb_fname->st.st_ex_size = get_xattr_size(handle->conn, NULL,
						  smb_fname->base_name,
						  xattr_name);
	if (smb_fname->st.st_ex_size == -1) {
		errno = ENOENT;
		result = -1;
		goto fail;
	}

	smb_fname->st.st_ex_ino = stream_inode(&smb_fname->st, xattr_name);
	smb_fname->st.st_ex_mode &= ~S_IFMT;
        smb_fname->st.st_ex_mode |= S_IFREG;
        smb_fname->st.st_ex_blocks =
	    smb_fname->st.st_ex_size / STAT_ST_BLOCKSIZE + 1;

	result = 0;

 fail:
	TALLOC_FREE(xattr_name);
	return result;
}

static int streams_xattr_open(vfs_handle_struct *handle,
			      struct smb_filename *smb_fname,
			      files_struct *fsp, int flags, mode_t mode)
{
	NTSTATUS status;
	struct smb_filename *smb_fname_base = NULL;
	struct stream_io *sio;
	struct ea_struct ea;
	char *xattr_name = NULL;
	int baseflags;
	int hostfd = -1;
	int ret;

	DEBUG(10, ("streams_xattr_open called for %s with flags 0x%x\n",
		   smb_fname_str_dbg(smb_fname), flags));

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

	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname->stream_name, &xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto fail;
	}

	/* Create an smb_filename with stream_name == NULL. */
	smb_fname_base = synthetic_smb_fname(
		talloc_tos(), smb_fname->base_name, NULL, NULL);
	if (smb_fname_base == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	/*
	 * We use baseflags to turn off nasty side-effects when opening the
	 * underlying file.
         */
        baseflags = flags;
        baseflags &= ~O_TRUNC;
        baseflags &= ~O_EXCL;
        baseflags &= ~O_CREAT;

        hostfd = SMB_VFS_OPEN(handle->conn, smb_fname_base, fsp,
			      baseflags, mode);

	TALLOC_FREE(smb_fname_base);

        /* It is legit to open a stream on a directory, but the base
         * fd has to be read-only.
         */
        if ((hostfd == -1) && (errno == EISDIR)) {
                baseflags &= ~O_ACCMODE;
                baseflags |= O_RDONLY;
                hostfd = SMB_VFS_OPEN(handle->conn, smb_fname, fsp, baseflags,
				      mode);
        }

        if (hostfd == -1) {
		goto fail;
        }

	status = get_ea_value(talloc_tos(), handle->conn, NULL,
			      smb_fname->base_name, xattr_name, &ea);

	DEBUG(10, ("get_ea_value returned %s\n", nt_errstr(status)));

	if (!NT_STATUS_IS_OK(status)
	    && !NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		/*
		 * The base file is not there. This is an error even if we got
		 * O_CREAT, the higher levels should have created the base
		 * file for us.
		 */
		DEBUG(10, ("streams_xattr_open: base file %s not around, "
			   "returning ENOENT\n", smb_fname->base_name));
		errno = ENOENT;
		goto fail;
	}

	if ((!NT_STATUS_IS_OK(status) && (flags & O_CREAT)) ||
	    (flags & O_TRUNC)) {
		/*
		 * The attribute does not exist or needs to be truncated
		 */

		/*
		 * Darn, xattrs need at least 1 byte
		 */
		char null = '\0';

		DEBUG(10, ("creating or truncating attribute %s on file %s\n",
			   xattr_name, smb_fname->base_name));

		fsp->fh->fd = hostfd;
		ret = SMB_VFS_FSETXATTR(fsp, xattr_name,
					&null, sizeof(null),
					flags & O_EXCL ? XATTR_CREATE : 0);
		fsp->fh->fd = -1;
		if (ret != 0) {
			goto fail;
		}
	}

        sio = (struct stream_io *)VFS_ADD_FSP_EXTENSION(handle, fsp,
							struct stream_io,
							NULL);
        if (sio == NULL) {
                errno = ENOMEM;
                goto fail;
        }

        sio->xattr_name = talloc_strdup(VFS_MEMCTX_FSP_EXTENSION(handle, fsp),
					xattr_name);
	/*
	 * so->base needs to be a copy of fsp->fsp_name->base_name,
	 * making it identical to streams_xattr_recheck(). If the
	 * open is changing directories, fsp->fsp_name->base_name
	 * will be the full path from the share root, whilst
	 * smb_fname will be relative to the $cwd.
	 */
        sio->base = talloc_strdup(VFS_MEMCTX_FSP_EXTENSION(handle, fsp),
				  fsp->fsp_name->base_name);
	sio->fsp_name_ptr = fsp->fsp_name;
	sio->handle = handle;
	sio->fsp = fsp;

	if ((sio->xattr_name == NULL) || (sio->base == NULL)) {
		errno = ENOMEM;
		goto fail;
	}

	return hostfd;

 fail:
	if (hostfd >= 0) {
		/*
		 * BUGBUGBUG -- we would need to call fd_close_posix here, but
		 * we don't have a full fsp yet
		 */
		fsp->fh->fd = hostfd;
		SMB_VFS_CLOSE(fsp);
	}

	return -1;
}

static int streams_xattr_unlink(vfs_handle_struct *handle,
				const struct smb_filename *smb_fname)
{
	NTSTATUS status;
	int ret = -1;
	char *xattr_name = NULL;

	if (!is_ntfs_stream_smb_fname(smb_fname)) {
		return SMB_VFS_NEXT_UNLINK(handle, smb_fname);
	}

	/* If the default stream is requested, just open the base file. */
	if (is_ntfs_default_stream_smb_fname(smb_fname)) {
		struct smb_filename *smb_fname_base = NULL;

		smb_fname_base = cp_smb_filename(talloc_tos(), smb_fname);
		if (smb_fname_base == NULL) {
			errno = ENOMEM;
			return -1;
		}

		ret = SMB_VFS_NEXT_UNLINK(handle, smb_fname_base);

		TALLOC_FREE(smb_fname_base);
		return ret;
	}

	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname->stream_name, &xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto fail;
	}

	ret = SMB_VFS_REMOVEXATTR(handle->conn, smb_fname->base_name, xattr_name);

	if ((ret == -1) && (errno == ENOATTR)) {
		errno = ENOENT;
		goto fail;
	}

	ret = 0;

 fail:
	TALLOC_FREE(xattr_name);
	return ret;
}

static int streams_xattr_rename(vfs_handle_struct *handle,
				const struct smb_filename *smb_fname_src,
				const struct smb_filename *smb_fname_dst)
{
	NTSTATUS status;
	int ret = -1;
	char *src_xattr_name = NULL;
	char *dst_xattr_name = NULL;
	bool src_is_stream, dst_is_stream;
	ssize_t oret;
	ssize_t nret;
	struct ea_struct ea;

	src_is_stream = is_ntfs_stream_smb_fname(smb_fname_src);
	dst_is_stream = is_ntfs_stream_smb_fname(smb_fname_dst);

	if (!src_is_stream && !dst_is_stream) {
		return SMB_VFS_NEXT_RENAME(handle, smb_fname_src,
					   smb_fname_dst);
	}

	/* For now don't allow renames from or to the default stream. */
	if (is_ntfs_default_stream_smb_fname(smb_fname_src) ||
	    is_ntfs_default_stream_smb_fname(smb_fname_dst)) {
		errno = ENOSYS;
		goto done;
	}

	/* Don't rename if the streams are identical. */
	if (strcasecmp_m(smb_fname_src->stream_name,
		       smb_fname_dst->stream_name) == 0) {
		goto done;
	}

	/* Get the xattr names. */
	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname_src->stream_name,
					&src_xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto fail;
	}
	status = streams_xattr_get_name(handle, talloc_tos(),
					smb_fname_dst->stream_name,
					&dst_xattr_name);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		goto fail;
	}

	/* read the old stream */
	status = get_ea_value(talloc_tos(), handle->conn, NULL,
			      smb_fname_src->base_name, src_xattr_name, &ea);
	if (!NT_STATUS_IS_OK(status)) {
		errno = ENOENT;
		goto fail;
	}

	/* (over)write the new stream */
	nret = SMB_VFS_SETXATTR(handle->conn, smb_fname_src->base_name,
				dst_xattr_name, ea.value.data, ea.value.length,
				0);
	if (nret < 0) {
		if (errno == ENOATTR) {
			errno = ENOENT;
		}
		goto fail;
	}

	/* remove the old stream */
	oret = SMB_VFS_REMOVEXATTR(handle->conn, smb_fname_src->base_name,
				   src_xattr_name);
	if (oret < 0) {
		if (errno == ENOATTR) {
			errno = ENOENT;
		}
		goto fail;
	}

 done:
	errno = 0;
	ret = 0;
 fail:
	TALLOC_FREE(src_xattr_name);
	TALLOC_FREE(dst_xattr_name);
	return ret;
}

static NTSTATUS walk_xattr_streams(vfs_handle_struct *handle, files_struct *fsp,
				   const char *fname,
				   bool (*fn)(struct ea_struct *ea,
					      void *private_data),
				   void *private_data)
{
	NTSTATUS status;
	char **names;
	size_t i, num_names;
	struct streams_xattr_config *config;

	SMB_VFS_HANDLE_GET_DATA(handle, config, struct streams_xattr_config,
				return NT_STATUS_UNSUCCESSFUL);

	status = get_ea_names_from_file(talloc_tos(), handle->conn, fsp, fname,
					&names, &num_names);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	for (i=0; i<num_names; i++) {
		struct ea_struct ea;

		/*
		 * We want to check with samba_private_attr_name()
		 * whether the xattr name is a private one,
		 * unfortunately it flags xattrs that begin with the
		 * default streams prefix as private.
		 *
		 * By only calling samba_private_attr_name() in case
		 * the xattr does NOT begin with the default prefix,
		 * we know that if it returns 'true' it definitely one
		 * of our internal xattr like "user.DOSATTRIB".
		 */
		if (strncasecmp_m(names[i], SAMBA_XATTR_DOSSTREAM_PREFIX,
				  strlen(SAMBA_XATTR_DOSSTREAM_PREFIX)) != 0) {
			if (samba_private_attr_name(names[i])) {
				continue;
			}
		}

		if (strncmp(names[i], config->prefix,
			    config->prefix_len) != 0) {
			continue;
		}

		status = get_ea_value(names, handle->conn, fsp, fname,
				      names[i], &ea);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(10, ("Could not get ea %s for file %s: %s\n",
				   names[i], fname, nt_errstr(status)));
			continue;
		}

		ea.name = talloc_asprintf(
			ea.value.data, ":%s%s",
			names[i] + config->prefix_len,
			config->store_stream_type ? "" : ":$DATA");
		if (ea.name == NULL) {
			DEBUG(0, ("talloc failed\n"));
			continue;
		}

		if (!fn(&ea, private_data)) {
			TALLOC_FREE(ea.value.data);
			return NT_STATUS_OK;
		}

		TALLOC_FREE(ea.value.data);
	}

	TALLOC_FREE(names);
	return NT_STATUS_OK;
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

static bool collect_one_stream(struct ea_struct *ea, void *private_data)
{
	struct streaminfo_state *state =
		(struct streaminfo_state *)private_data;

	if (!add_one_stream(state->mem_ctx,
			    &state->num_streams, &state->streams,
			    ea->name, ea->value.length-1,
			    smb_roundup(state->handle->conn,
					ea->value.length-1))) {
		state->status = NT_STATUS_NO_MEMORY;
		return false;
	}

	return true;
}

static NTSTATUS streams_xattr_streaminfo(vfs_handle_struct *handle,
					 struct files_struct *fsp,
					 const char *fname,
					 TALLOC_CTX *mem_ctx,
					 unsigned int *pnum_streams,
					 struct stream_struct **pstreams)
{
	SMB_STRUCT_STAT sbuf;
	int ret;
	NTSTATUS status;
	struct streaminfo_state state;

	if ((fsp != NULL) && (fsp->fh->fd != -1)) {
		ret = SMB_VFS_FSTAT(fsp, &sbuf);
	}
	else {
		struct smb_filename *smb_fname = NULL;
		smb_fname = synthetic_smb_fname(talloc_tos(), fname, NULL,
						NULL);
		if (smb_fname == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
		if (lp_posix_pathnames()) {
			ret = SMB_VFS_LSTAT(handle->conn, smb_fname);
		} else {
			ret = SMB_VFS_STAT(handle->conn, smb_fname);
		}
		sbuf = smb_fname->st;
		TALLOC_FREE(smb_fname);
	}

	if (ret == -1) {
		return map_nt_error_from_unix(errno);
	}

	state.streams = *pstreams;
	state.num_streams = *pnum_streams;
	state.mem_ctx = mem_ctx;
	state.handle = handle;
	state.status = NT_STATUS_OK;

	if (S_ISLNK(sbuf.st_ex_mode)) {
		/*
		 * Currently we do't have SMB_VFS_LLISTXATTR
		 * inside the VFS which means there's no way
		 * to cope with a symlink when lp_posix_pathnames().
		 * returns true. For now ignore links.
		 * FIXME - by adding SMB_VFS_LLISTXATTR. JRA.
		 */
		status = NT_STATUS_OK;
	} else {
		status = walk_xattr_streams(handle, fsp, fname,
				    collect_one_stream, &state);
	}

	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(state.streams);
		return status;
	}

	if (!NT_STATUS_IS_OK(state.status)) {
		TALLOC_FREE(state.streams);
		return state.status;
	}

	*pnum_streams = state.num_streams;
	*pstreams = state.streams;

	return SMB_VFS_NEXT_STREAMINFO(handle, fsp, fname, mem_ctx, pnum_streams, pstreams);
}

static uint32_t streams_xattr_fs_capabilities(struct vfs_handle_struct *handle,
			enum timestamp_set_resolution *p_ts_res)
{
	return SMB_VFS_NEXT_FS_CAPABILITIES(handle, p_ts_res) | FILE_NAMED_STREAMS;
}

static int streams_xattr_connect(vfs_handle_struct *handle,
				 const char *service, const char *user)
{
	struct streams_xattr_config *config;
	const char *default_prefix = SAMBA_XATTR_DOSSTREAM_PREFIX;
	const char *prefix;
	int rc;

	rc = SMB_VFS_NEXT_CONNECT(handle, service, user);
	if (rc != 0) {
		return rc;
	}

	config = talloc_zero(handle->conn, struct streams_xattr_config);
	if (config == NULL) {
		DEBUG(1, ("talloc_zero() failed\n"));
		errno = ENOMEM;
		return -1;
	}

	prefix = lp_parm_const_string(SNUM(handle->conn),
				      "streams_xattr", "prefix",
				      default_prefix);
	config->prefix = talloc_strdup(config, prefix);
	if (config->prefix == NULL) {
		DEBUG(1, ("talloc_strdup() failed\n"));
		errno = ENOMEM;
		return -1;
	}
	config->prefix_len = strlen(config->prefix);
	DEBUG(10, ("streams_xattr using stream prefix: %s\n", config->prefix));

	config->store_stream_type = lp_parm_bool(SNUM(handle->conn),
						 "streams_xattr",
						 "store_stream_type",
						 true);

	SMB_VFS_HANDLE_SET_DATA(handle, config,
				NULL, struct stream_xattr_config,
				return -1);

	return 0;
}

static ssize_t streams_xattr_pwrite(vfs_handle_struct *handle,
				    files_struct *fsp, const void *data,
				    size_t n, off_t offset)
{
        struct stream_io *sio =
		(struct stream_io *)VFS_FETCH_FSP_EXTENSION(handle, fsp);
	struct ea_struct ea;
	NTSTATUS status;
	int ret;

	DEBUG(10, ("streams_xattr_pwrite called for %d bytes\n", (int)n));

	if (sio == NULL) {
		return SMB_VFS_NEXT_PWRITE(handle, fsp, data, n, offset);
	}

	if (!streams_xattr_recheck(sio)) {
		return -1;
	}

	status = get_ea_value(talloc_tos(), handle->conn, fsp,
			      sio->base, sio->xattr_name, &ea);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

        if ((offset + n) > ea.value.length-1) {
		uint8_t *tmp;

		tmp = talloc_realloc(talloc_tos(), ea.value.data, uint8_t,
					   offset + n + 1);

		if (tmp == NULL) {
			TALLOC_FREE(ea.value.data);
                        errno = ENOMEM;
                        return -1;
                }
		ea.value.data = tmp;
		ea.value.length = offset + n + 1;
		ea.value.data[offset+n] = 0;
        }

        memcpy(ea.value.data + offset, data, n);

	if (fsp->fh->fd != -1) {
		ret = SMB_VFS_FSETXATTR(fsp,
				sio->xattr_name,
				ea.value.data, ea.value.length, 0);
	} else {
		ret = SMB_VFS_SETXATTR(fsp->conn,
				       fsp->fsp_name->base_name,
				sio->xattr_name,
				ea.value.data, ea.value.length, 0);
	}
	TALLOC_FREE(ea.value.data);

	if (ret == -1) {
		return -1;
	}

	return n;
}

static ssize_t streams_xattr_pread(vfs_handle_struct *handle,
				   files_struct *fsp, void *data,
				   size_t n, off_t offset)
{
        struct stream_io *sio =
		(struct stream_io *)VFS_FETCH_FSP_EXTENSION(handle, fsp);
	struct ea_struct ea;
	NTSTATUS status;
	size_t length, overlap;

	DEBUG(10, ("streams_xattr_pread: offset=%d, size=%d\n",
		   (int)offset, (int)n));

	if (sio == NULL) {
		return SMB_VFS_NEXT_PREAD(handle, fsp, data, n, offset);
	}

	if (!streams_xattr_recheck(sio)) {
		return -1;
	}

	status = get_ea_value(talloc_tos(), handle->conn, fsp,
			      sio->base, sio->xattr_name, &ea);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

	length = ea.value.length-1;

	DEBUG(10, ("streams_xattr_pread: get_ea_value returned %d bytes\n",
		   (int)length));

        /* Attempt to read past EOF. */
        if (length <= offset) {
                return 0;
        }

        overlap = (offset + n) > length ? (length - offset) : n;
        memcpy(data, ea.value.data + offset, overlap);

	TALLOC_FREE(ea.value.data);
        return overlap;
}

static int streams_xattr_ftruncate(struct vfs_handle_struct *handle,
					struct files_struct *fsp,
					off_t offset)
{
	int ret;
	uint8_t *tmp;
	struct ea_struct ea;
	NTSTATUS status;
        struct stream_io *sio =
		(struct stream_io *)VFS_FETCH_FSP_EXTENSION(handle, fsp);

	DEBUG(10, ("streams_xattr_ftruncate called for file %s offset %.0f\n",
		   fsp_str_dbg(fsp), (double)offset));

	if (sio == NULL) {
		return SMB_VFS_NEXT_FTRUNCATE(handle, fsp, offset);
	}

	if (!streams_xattr_recheck(sio)) {
		return -1;
	}

	status = get_ea_value(talloc_tos(), handle->conn, fsp,
			      sio->base, sio->xattr_name, &ea);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

	tmp = talloc_realloc(talloc_tos(), ea.value.data, uint8_t,
				   offset + 1);

	if (tmp == NULL) {
		TALLOC_FREE(ea.value.data);
		errno = ENOMEM;
		return -1;
	}

	/* Did we expand ? */
	if (ea.value.length < offset + 1) {
		memset(&tmp[ea.value.length], '\0',
			offset + 1 - ea.value.length);
	}

	ea.value.data = tmp;
	ea.value.length = offset + 1;
	ea.value.data[offset] = 0;

	if (fsp->fh->fd != -1) {
		ret = SMB_VFS_FSETXATTR(fsp,
				sio->xattr_name,
				ea.value.data, ea.value.length, 0);
	} else {
		ret = SMB_VFS_SETXATTR(fsp->conn,
				fsp->fsp_name->base_name,
				sio->xattr_name,
				ea.value.data, ea.value.length, 0);
	}

	TALLOC_FREE(ea.value.data);

	if (ret == -1) {
		return -1;
	}

	return 0;
}

static int streams_xattr_fallocate(struct vfs_handle_struct *handle,
					struct files_struct *fsp,
					uint32_t mode,
					off_t offset,
					off_t len)
{
        struct stream_io *sio =
		(struct stream_io *)VFS_FETCH_FSP_EXTENSION(handle, fsp);

	DEBUG(10, ("streams_xattr_fallocate called for file %s offset %.0f"
		"len = %.0f\n",
		fsp_str_dbg(fsp), (double)offset, (double)len));

	if (sio == NULL) {
		return SMB_VFS_NEXT_FALLOCATE(handle, fsp, mode, offset, len);
	}

	if (!streams_xattr_recheck(sio)) {
		return -1;
	}

	/* Let the pwrite code path handle it. */
	errno = ENOSYS;
	return -1;
}


static struct vfs_fn_pointers vfs_streams_xattr_fns = {
	.fs_capabilities_fn = streams_xattr_fs_capabilities,
	.connect_fn = streams_xattr_connect,
	.open_fn = streams_xattr_open,
	.stat_fn = streams_xattr_stat,
	.fstat_fn = streams_xattr_fstat,
	.lstat_fn = streams_xattr_lstat,
	.pread_fn = streams_xattr_pread,
	.pwrite_fn = streams_xattr_pwrite,
	.unlink_fn = streams_xattr_unlink,
	.rename_fn = streams_xattr_rename,
	.ftruncate_fn = streams_xattr_ftruncate,
	.fallocate_fn = streams_xattr_fallocate,
	.streaminfo_fn = streams_xattr_streaminfo,
};

NTSTATUS vfs_streams_xattr_init(void);
NTSTATUS vfs_streams_xattr_init(void)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "streams_xattr",
				&vfs_streams_xattr_fns);
}
