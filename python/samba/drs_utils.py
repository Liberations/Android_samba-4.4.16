# DRS utility code
#
# Copyright Andrew Tridgell 2010
# Copyright Andrew Bartlett 2010
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

from samba.dcerpc import drsuapi, misc
from samba.net import Net
import samba, ldb


class drsException(Exception):
    """Base element for drs errors"""

    def __init__(self, value):
        self.value = value

    def __str__(self):
        return "drsException: " + self.value


def drsuapi_connect(server, lp, creds):
    """Make a DRSUAPI connection to the server.

    :param server: the name of the server to connect to
    :param lp: a samba line parameter object
    :param creds: credential used for the connection
    :return: A tuple with the drsuapi bind object, the drsuapi handle
                and the supported extensions.
    :raise drsException: if the connection fails
    """

    binding_options = "seal"
    if lp.log_level() >= 5:
        binding_options += ",print"
    binding_string = "ncacn_ip_tcp:%s[%s]" % (server, binding_options)
    try:
        drsuapiBind = drsuapi.drsuapi(binding_string, lp, creds)
        (drsuapiHandle, bindSupportedExtensions) = drs_DsBind(drsuapiBind)
    except Exception, e:
        raise drsException("DRS connection to %s failed: %s" % (server, e))

    return (drsuapiBind, drsuapiHandle, bindSupportedExtensions)


def sendDsReplicaSync(drsuapiBind, drsuapi_handle, source_dsa_guid,
        naming_context, req_option):
    """Send DS replica sync request.

    :param drsuapiBind: a drsuapi Bind object
    :param drsuapi_handle: a drsuapi hanle on the drsuapi connection
    :param source_dsa_guid: the guid of the source dsa for the replication
    :param naming_context: the DN of the naming context to replicate
    :param req_options: replication options for the DsReplicaSync call
    :raise drsException: if any error occur while sending and receiving the
        reply for the dsReplicaSync
    """

    nc = drsuapi.DsReplicaObjectIdentifier()
    nc.dn = naming_context

    req1 = drsuapi.DsReplicaSyncRequest1()
    req1.naming_context = nc;
    req1.options = req_option
    req1.source_dsa_guid = misc.GUID(source_dsa_guid)

    try:
        drsuapiBind.DsReplicaSync(drsuapi_handle, 1, req1)
    except Exception, estr:
        raise drsException("DsReplicaSync failed %s" % estr)


def sendRemoveDsServer(drsuapiBind, drsuapi_handle, server_dsa_dn, domain):
    """Send RemoveDSServer request.

    :param drsuapiBind: a drsuapi Bind object
    :param drsuapi_handle: a drsuapi hanle on the drsuapi connection
    :param server_dsa_dn: a DN object of the server's dsa that we want to
        demote
    :param domain: a DN object of the server's domain
    :raise drsException: if any error occur while sending and receiving the
        reply for the DsRemoveDSServer
    """

    try:
        req1 = drsuapi.DsRemoveDSServerRequest1()
        req1.server_dn = str(server_dsa_dn)
        req1.domain_dn = str(domain)
        req1.commit = 1

        drsuapiBind.DsRemoveDSServer(drsuapi_handle, 1, req1)
    except Exception, estr:
        raise drsException("DsRemoveDSServer failed %s" % estr)


def drs_DsBind(drs):
    '''make a DsBind call, returning the binding handle'''
    bind_info = drsuapi.DsBindInfoCtr()
    bind_info.length = 28
    bind_info.info = drsuapi.DsBindInfo28()
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_BASE
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_ASYNC_REPLICATION
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_REMOVEAPI
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_MOVEREQ_V2
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHG_COMPRESS
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_DCINFO_V1
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_RESTORE_USN_OPTIMIZATION
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_KCC_EXECUTE
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_ADDENTRY_V2
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_LINKED_VALUE_REPLICATION
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_DCINFO_V2
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_INSTANCE_TYPE_NOT_REQ_ON_MOD
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_CRYPTO_BIND
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GET_REPL_INFO
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_STRONG_ENCRYPTION
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_DCINFO_V01
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_TRANSITIVE_MEMBERSHIP
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_ADD_SID_HISTORY
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_POST_BETA3
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GET_MEMBERSHIPS2
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREQ_V6
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_NONDOMAIN_NCS
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREQ_V8
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREPLY_V5
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREPLY_V6
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_ADDENTRYREPLY_V3
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREPLY_V7
    bind_info.info.supported_extensions |= drsuapi.DRSUAPI_SUPPORTED_EXTENSION_VERIFY_OBJECT
    (info, handle) = drs.DsBind(misc.GUID(drsuapi.DRSUAPI_DS_BIND_GUID), bind_info)

    return (handle, info.info.supported_extensions)


class drs_Replicate(object):
    '''DRS replication calls'''

    def __init__(self, binding_string, lp, creds, samdb, invocation_id):
        self.drs = drsuapi.drsuapi(binding_string, lp, creds)
        (self.drs_handle, self.supported_extensions) = drs_DsBind(self.drs)
        self.net = Net(creds=creds, lp=lp)
        self.samdb = samdb
        if not isinstance(invocation_id, misc.GUID):
            raise RuntimeError("Must supply GUID for invocation_id")
        if invocation_id == misc.GUID("00000000-0000-0000-0000-000000000000"):
            raise RuntimeError("Must not set GUID 00000000-0000-0000-0000-000000000000 as invocation_id")
        self.replication_state = self.net.replicate_init(self.samdb, lp, self.drs, invocation_id)

    def drs_get_rodc_partial_attribute_set(self):
        '''get a list of attributes for RODC replication'''
        partial_attribute_set = drsuapi.DsPartialAttributeSet()
        partial_attribute_set.version = 1

        attids = []

        # the exact list of attids we send is quite critical. Note that
        # we do ask for the secret attributes, but set SPECIAL_SECRET_PROCESSING
        # to zero them out
        schema_dn = self.samdb.get_schema_basedn()
        res = self.samdb.search(base=schema_dn, scope=ldb.SCOPE_SUBTREE,
                                      expression="objectClass=attributeSchema",
                                      attrs=["lDAPDisplayName", "systemFlags",
                                             "searchFlags"])

        for r in res:
            ldap_display_name = r["lDAPDisplayName"][0]
            if "systemFlags" in r:
                system_flags      = r["systemFlags"][0]
                if (int(system_flags) & (samba.dsdb.DS_FLAG_ATTR_NOT_REPLICATED |
                                         samba.dsdb.DS_FLAG_ATTR_IS_CONSTRUCTED)):
                    continue
            if "searchFlags" in r:
                search_flags = r["searchFlags"][0]
                if (int(search_flags) & samba.dsdb.SEARCH_FLAG_RODC_ATTRIBUTE):
                    continue
            attid = self.samdb.get_attid_from_lDAPDisplayName(ldap_display_name)
            attids.append(int(attid))

        # the attids do need to be sorted, or windows doesn't return
        # all the attributes we need
        attids.sort()
        partial_attribute_set.attids         = attids
        partial_attribute_set.num_attids = len(attids)
        return partial_attribute_set

    def replicate(self, dn, source_dsa_invocation_id, destination_dsa_guid,
                  schema=False, exop=drsuapi.DRSUAPI_EXOP_NONE, rodc=False,
                  replica_flags=None):
        '''replicate a single DN'''

        # setup for a GetNCChanges call
        req8 = drsuapi.DsGetNCChangesRequest8()

        req8.destination_dsa_guid = destination_dsa_guid
        req8.source_dsa_invocation_id = source_dsa_invocation_id
        req8.naming_context = drsuapi.DsReplicaObjectIdentifier()
        req8.naming_context.dn = dn
        req8.highwatermark = drsuapi.DsReplicaHighWaterMark()
        req8.highwatermark.tmp_highest_usn = 0
        req8.highwatermark.reserved_usn = 0
        req8.highwatermark.highest_usn = 0
        req8.uptodateness_vector = None
        if replica_flags is not None:
            req8.replica_flags = replica_flags
        elif exop == drsuapi.DRSUAPI_EXOP_REPL_SECRET:
            req8.replica_flags = 0
        else:
            req8.replica_flags = (drsuapi.DRSUAPI_DRS_INIT_SYNC |
                                  drsuapi.DRSUAPI_DRS_PER_SYNC |
                                  drsuapi.DRSUAPI_DRS_GET_ANC |
                                  drsuapi.DRSUAPI_DRS_NEVER_SYNCED)
            if rodc:
                req8.replica_flags |= (
                    drsuapi.DRSUAPI_DRS_SPECIAL_SECRET_PROCESSING)
            else:
                req8.replica_flags |= drsuapi.DRSUAPI_DRS_WRIT_REP
        req8.max_object_count = 402
        req8.max_ndr_size = 402116
        req8.extended_op = exop
        req8.fsmo_info = 0
        req8.partial_attribute_set = None
        req8.partial_attribute_set_ex = None
        req8.mapping_ctr.num_mappings = 0
        req8.mapping_ctr.mappings = None

        if not schema and rodc:
            req8.partial_attribute_set = self.drs_get_rodc_partial_attribute_set()

        if self.supported_extensions & drsuapi.DRSUAPI_SUPPORTED_EXTENSION_GETCHGREQ_V8:
            req_level = 8
            req = req8
        else:
            req_level = 5
            req5 = drsuapi.DsGetNCChangesRequest5()
            for a in dir(req5):
                if a[0] != '_':
                    setattr(req5, a, getattr(req8, a))
            req = req5

        while True:
            (level, ctr) = self.drs.DsGetNCChanges(self.drs_handle, req_level, req)
            if ctr.first_object is None and ctr.object_count != 0:
                raise RuntimeError("DsGetNCChanges: NULL first_object with object_count=%u" % (ctr.object_count))
            self.net.replicate_chunk(self.replication_state, level, ctr,
                schema=schema, req_level=req_level, req=req)
            if ctr.more_data == 0:
                break
            req.highwatermark = ctr.new_highwatermark
