/* SPDX-License-Identifier: LGPL-2.1+ */
/*
 *   Copyright (c) International Business Machines  Corp., 2007
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *   Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *   Modified by Namjae Jeon (linkinjeon@kernel.org)
 */

#include <smbacl.h>
#include <ksmbdtools.h>

static const struct smb_sid sid_domain = {1, 1, {0, 0, 0, 0, 0, 5},
	{21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

/* security id for everyone/world system group */
static const struct smb_sid sid_everyone = {
	1, 1, {0, 0, 0, 0, 0, 1}, {0} };
 
/* security id for local group */
static const struct smb_sid sid_local_group = {
	1, 1, {0, 0, 0, 0, 0, 5}, {32} };

/* S-1-22-2 Unmapped Unix groups */
static const struct smb_sid sid_unix_groups = { 1, 1, {0, 0, 0, 0, 0, 22},
		{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

void smb_read_sid(struct ksmbd_dcerpc *dce, struct smb_sid *sid)
{
	int i;

	sid->revision = ndr_read_int8(dce);
	sid->num_subauth = ndr_read_int8(dce);
	for (i = 0; i < NUM_AUTHS; ++i)
		sid->authority[i] = ndr_read_int8(dce);
	for (i = 0; i < sid->num_subauth; ++i)
		sid->sub_auth[i] = ndr_read_int32(dce);
}

void smb_write_sid(struct ksmbd_dcerpc *dce, const struct smb_sid *src)
{
	int i;

	ndr_write_int8(dce, src->revision);
	ndr_write_int8(dce, src->num_subauth);
	for (i = 0; i < NUM_AUTHS; ++i)
		ndr_write_int8(dce, src->authority[i]);
	for (i = 0; i < src->num_subauth; ++i)
		ndr_write_int32(dce, src->sub_auth[i]);
}

void smb_copy_sid(struct smb_sid *dst, const struct smb_sid *src)
{
	int i;

	dst->revision = src->revision;
	dst->num_subauth = src->num_subauth;
	for (i = 0; i < NUM_AUTHS; ++i)
		dst->authority[i] = src->authority[i];
	for (i = 0; i < dst->num_subauth; ++i)
		dst->sub_auth[i] = src->sub_auth[i];
}

void smb_init_domain_sid(struct smb_sid *sid)
{
	int i;

	sid->revision = 1;
	sid->num_subauth = 4;
	sid->authority[5] = 5;
	sid->sub_auth[0] = 21;
	for (i = 0; i < 3; i++)
		sid->sub_auth[i+1] = global_conf.sub_auth[i];
}

int smb_compare_sids(const struct smb_sid *ctsid, const struct smb_sid *cwsid)
{
	int i;
	int num_subauth, num_sat, num_saw;

	if ((!ctsid) || (!cwsid))
		return 1;

	/* compare the revision */
	if (ctsid->revision != cwsid->revision) {
		if (ctsid->revision > cwsid->revision)
			return 1;
		else
			return -1;
	}

	/* compare all of the six auth values */
	for (i = 0; i < NUM_AUTHS; ++i) {
		if (ctsid->authority[i] != cwsid->authority[i]) {
			if (ctsid->authority[i] > cwsid->authority[i])
				return 1;
			else
				return -1;
		}
	}

	/* compare all of the subauth values if any */
	num_sat = ctsid->num_subauth;
	num_saw = cwsid->num_subauth;
	num_subauth = num_sat < num_saw ? num_sat : num_saw;
	if (num_subauth) {
		for (i = 0; i < num_subauth; ++i) {
			if (ctsid->sub_auth[i] != cwsid->sub_auth[i]) {
				if (ctsid->sub_auth[i] >
					cwsid->sub_auth[i])
					return 1;
				else
					return -1;
			}
		}
	}

	return 0; /* sids compare/match */
}

int get_sid_info(struct smb_sid *sid, int *sid_type, char *domain_name)
{
	int ret = 0;

	if (!smb_compare_sids(sid, &sid_domain)) {
		*sid_type = SID_TYPE_USER;
		gethostname(domain_name, 256);
	} else if (!smb_compare_sids(sid, &sid_unix_groups)) {
		*sid_type = SID_TYPE_GROUP;
		strcpy(domain_name, "Unix Group");
	} else
		ret = -ENOENT;

	return ret;
}

static int smb_set_ace(struct ksmbd_dcerpc *dce, int access_req, int rid,
		const struct smb_sid *rsid)
{
	int size;
	struct smb_sid sid = {0};

	memcpy(&sid, rsid, sizeof(struct smb_sid));
	ndr_write_int8(dce, ACCESS_ALLOWED); // ace type
	ndr_write_int8(dce, 0); // ace flags
	size = 1 + 1 + 2 + 4 + 1 + 1 + 6 + (sid.num_subauth * 4);
	if (rid)
		size += 4;
	ndr_write_int16(dce, size); // ace size
	ndr_write_int32(dce, access_req); // ace access required
	if (rid)
		sid.sub_auth[sid.num_subauth++] = rid;
	smb_write_sid(dce, &sid);

	return size;
}

static int set_dacl(struct ksmbd_dcerpc *dce, int rid)
{
	int size = 0, i;
	struct smb_sid owner_domain;

	/* Other */
	size += smb_set_ace(dce, 0x0002035b, 0, &sid_everyone);
	/* Local Group Administrators */
	size += smb_set_ace(dce, 0x000f07ff, 544, &sid_local_group);
	/* Local Group Account Operators */
	size += smb_set_ace(dce, 0x000f07ff, 513, &sid_local_group);
	/* Owner RID */
	memcpy(&owner_domain, &sid_domain, sizeof(struct smb_sid));
	for (i = 0; i < 3; ++i) {
		owner_domain.sub_auth[i + 1] = global_conf.sub_auth[i];
		owner_domain.num_subauth++;
	}
	size += smb_set_ace(dce, 0x00020044, rid, &owner_domain);

	return size;
}

int build_sec_desc(struct ksmbd_dcerpc *dce, __u32 *secdesclen, int rid)
{
	__u32 offset;
	int l_offset, acl_size_offset;
	int acl_size;

	/* NT Security Descrptor : Revision */
	ndr_write_int16(dce, 1);

	/* ACL Type */
	ndr_write_int16(dce, SELF_RELATIVE | DACL_PRESENT);

	/* Offset to owner SID */
	ndr_write_int32(dce, 0);
	/* Offset to group SID */
	ndr_write_int32(dce, 0);
	/* Offset to SACL */
	ndr_write_int32(dce, 0);
	/* Offset to DACL */
	ndr_write_int32(dce, sizeof(struct smb_ntsd));

	/* DACL Revsion */
	ndr_write_int16(dce, 2);
	acl_size_offset = dce->offset;
	dce->offset += 2;

	/* Number of ACEs */	
	ndr_write_int32(dce, 4);
	
	acl_size = set_dacl(dce, rid) + sizeof(struct smb_acl);
	/* ACL Size */
	l_offset = dce->offset;
	dce->offset = acl_size_offset;
	ndr_write_int16(dce, acl_size);
	dce->offset = l_offset;

	*secdesclen = sizeof(struct smb_ntsd) + acl_size;
	return 0;
}
