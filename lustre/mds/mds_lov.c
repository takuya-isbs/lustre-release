/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/mds/mds_lov.c
 *  Lustre Metadata Server (mds) handling of striped file data
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <linux/lustre_mds.h>
#include <linux/obd_ost.h>
#include <linux/lustre_idl.h>
#include <linux/obd_class.h>
#include <linux/obd_lov.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_fsfilt.h>

#include "mds_internal.h"

void le_lov_desc_to_cpu (struct lov_desc *ld)
{
        ld->ld_tgt_count = le32_to_cpu (ld->ld_tgt_count);
        ld->ld_default_stripe_count = le32_to_cpu (ld->ld_default_stripe_count);
        ld->ld_default_stripe_size = le32_to_cpu (ld->ld_default_stripe_size);
        ld->ld_pattern = le32_to_cpu (ld->ld_pattern);
}

void cpu_to_le_lov_desc (struct lov_desc *ld)
{
        ld->ld_tgt_count = cpu_to_le32 (ld->ld_tgt_count);
        ld->ld_default_stripe_count = cpu_to_le32 (ld->ld_default_stripe_count);
        ld->ld_default_stripe_size = cpu_to_le32 (ld->ld_default_stripe_size);
        ld->ld_pattern = cpu_to_le32 (ld->ld_pattern);
}

void mds_lov_update_objids(struct obd_device *obd, obd_id *ids)
{
        struct mds_obd *mds = &obd->u.mds;
        int i;
        ENTRY;

        lock_kernel();
        for (i = 0; i < mds->mds_lov_desc.ld_tgt_count; i++)
                if (ids[i] > (mds->mds_lov_objids)[i])
                        (mds->mds_lov_objids)[i] = ids[i];
        unlock_kernel();
        EXIT;
}

static int mds_lov_read_objids(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        obd_id *ids;
        loff_t off = 0;
        int i, rc, size = mds->mds_lov_desc.ld_tgt_count * sizeof(*ids);
        ENTRY;

        if (mds->mds_lov_objids != NULL)
                RETURN(0);

        OBD_ALLOC(ids, size);
        if (ids == NULL)
                RETURN(-ENOMEM);
        mds->mds_lov_objids = ids;

        if (mds->mds_lov_objid_filp->f_dentry->d_inode->i_size == 0)
                RETURN(0);
        rc = fsfilt_read_record(obd, mds->mds_lov_objid_filp, ids, size, &off);
        if (rc < 0) {
                CERROR("Error reading objids %d\n", rc);
        } else {
                mds->mds_lov_objids_valid = 1;
                rc = 0;
        }

        for (i = 0; i < mds->mds_lov_desc.ld_tgt_count; i++)
                CDEBUG(D_INFO, "read last object "LPU64" for idx %d\n",
                       mds->mds_lov_objids[i], i);

        RETURN(rc);
}

int mds_lov_write_objids(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        loff_t off = 0;
        int i, rc, size = mds->mds_lov_desc.ld_tgt_count * sizeof(obd_id);
        ENTRY;

        for (i = 0; i < mds->mds_lov_desc.ld_tgt_count; i++)
                CDEBUG(D_INFO, "writing last object "LPU64" for idx %d\n",
                       mds->mds_lov_objids[i], i);

        rc = fsfilt_write_record(obd, mds->mds_lov_objid_filp,
                                 mds->mds_lov_objids, size, &off, 0);
        RETURN(rc);
}

int mds_lov_clearorphans(struct mds_obd *mds, struct obd_uuid *ost_uuid)
{
        int rc;
        struct obdo oa;
        struct obd_trans_info oti = {0};
        struct lov_stripe_md  *empty_ea = NULL;
        ENTRY;

        LASSERT(mds->mds_lov_objids != NULL);

        /* This create will in fact either create or destroy:  If the OST is
         * missing objects below this ID, they will be created.  If it finds
         * objects above this ID, they will be removed. */
        memset(&oa, 0, sizeof(oa));
        oa.o_gr = FILTER_GROUP_FIRST_MDS + mds->mds_num;
        oa.o_valid = OBD_MD_FLFLAGS | OBD_MD_FLGROUP;
        oa.o_flags = OBD_FL_DELORPHAN;
        if (ost_uuid != NULL) {
                memcpy(&oa.o_inline, ost_uuid, sizeof(*ost_uuid));
                oa.o_valid |= OBD_MD_FLINLINE;
        }
        rc = obd_create(mds->mds_osc_exp, &oa, &empty_ea, &oti);

        RETURN(rc);
}

/* update the LOV-OSC knowledge of the last used object id's */
int mds_lov_set_nextid(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc;
        ENTRY;

        LASSERT(!obd->obd_recovering);

        LASSERT(mds->mds_lov_objids != NULL);

        rc = obd_set_info(mds->mds_osc_exp, strlen("next_id"), "next_id",
                          mds->mds_lov_desc.ld_tgt_count, mds->mds_lov_objids);
        RETURN(rc);
}

/* tell the LOV-OSC by how much to pre-create */
int mds_lov_set_growth(struct mds_obd *mds, int count)
{
        int rc;
        ENTRY;

        rc = obd_set_info(mds->mds_osc_exp, strlen("growth_count"),
                          "growth_count", sizeof(count), &count);

        RETURN(rc);
}

static int mds_lov_update_desc(struct obd_device *obd, struct obd_export *lov)
{
        struct mds_obd *mds = &obd->u.mds;
        int valsize = sizeof(mds->mds_lov_desc), rc, i;
        ENTRY;

        rc = obd_get_info(lov, strlen("lovdesc") + 1, "lovdesc", &valsize,
                          &mds->mds_lov_desc);
        if (rc)
                RETURN(rc);

        i = lov_mds_md_size(mds->mds_lov_desc.ld_tgt_count);
        if (i > mds->mds_max_mdsize)
                mds->mds_max_mdsize = i;
        mds->mds_max_cookiesize = mds->mds_lov_desc.ld_tgt_count *
                sizeof(struct llog_cookie);
        mds->mds_has_lov_desc = 1;
        RETURN(0);
}

int mds_lov_connect(struct obd_device *obd, char * lov_name)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_handle conn = {0,};
        char name[32] = "CATLIST";
        int rc, i, valsize;
        __u32 group;
        ENTRY;

        if (IS_ERR(mds->mds_osc_obd))
                RETURN(PTR_ERR(mds->mds_osc_obd));

        if (mds->mds_osc_obd)
                RETURN(0);

        mds->mds_osc_obd = class_name2obd(lov_name);
        if (!mds->mds_osc_obd) {
                CERROR("MDS cannot locate LOV %s\n",
                       lov_name);
                mds->mds_osc_obd = ERR_PTR(-ENOTCONN);
                RETURN(-ENOTCONN);
        }

        CDEBUG(D_HA, "obd: %s osc: %s lov_name: %s\n",
               obd->obd_name, mds->mds_osc_obd->obd_name, lov_name);

        rc = obd_connect(&conn, mds->mds_osc_obd, &obd->obd_uuid);
        if (rc) {
                CERROR("MDS cannot connect to LOV %s (%d)\n",
                       lov_name, rc);
                mds->mds_osc_obd = ERR_PTR(rc);
                RETURN(rc);
        }
        mds->mds_osc_exp = class_conn2export(&conn);

        rc = obd_register_observer(mds->mds_osc_obd, obd);
        if (rc) {
                CERROR("MDS cannot register as observer of LOV %s (%d)\n",
                       lov_name, rc);
                GOTO(err_discon, rc);
        }

        rc = mds_lov_update_desc(obd, mds->mds_osc_exp);
        if (rc)
                GOTO(err_reg, rc);

        rc = mds_lov_read_objids(obd);
        if (rc) {
                CERROR("cannot read %s: rc = %d\n", "lov_objids", rc);
                GOTO(err_reg, rc);
        }

        rc = obd_llog_cat_initialize(obd, &obd->obd_llogs, 
                                     mds->mds_lov_desc.ld_tgt_count, name);
        if (rc) {
                CERROR("failed to initialize catalog %d\n", rc);
                GOTO(err_reg, rc);
        }

        /* FIXME before set info call is made, we must initialize logging */
        group = FILTER_GROUP_FIRST_MDS + mds->mds_num;
        valsize = sizeof(group);
        rc = obd_set_info(mds->mds_osc_exp, strlen("mds_conn"), "mds_conn",
                          valsize, &group);
        if (rc)
                GOTO(err_reg, rc);

        /* If we're mounting this code for the first time on an existing FS,
         * we need to populate the objids array from the real OST values */
        if (!mds->mds_lov_objids_valid) {
                int size = sizeof(obd_id) * mds->mds_lov_desc.ld_tgt_count;
                rc = obd_get_info(mds->mds_osc_exp, strlen("last_id"),
                                  "last_id", &size, mds->mds_lov_objids);
                if (!rc) {
                        for (i = 0; i < mds->mds_lov_desc.ld_tgt_count; i++)
                                CWARN("got last object "LPU64" from OST %d\n",
                                      mds->mds_lov_objids[i], i);
                        mds->mds_lov_objids_valid = 1;
                        rc = mds_lov_write_objids(obd);
                        if (rc)
                                CERROR("got last objids from OSTs, but error "
                                       "writing objids file: %d\n", rc);
                }
        }

        /* I want to see a callback happen when the OBD moves to a
         * "For General Use" state, and that's when we'll call
         * set_nextid().  The class driver can help us here, because
         * it can use the obd_recovering flag to determine when the
         * the OBD is full available. */
        if (!obd->obd_recovering)
                rc = mds_postrecov(obd);
        RETURN(rc);

err_reg:
        obd_register_observer(mds->mds_osc_obd, NULL);
err_discon:
        obd_disconnect(mds->mds_osc_exp, 0);
        mds->mds_osc_exp = NULL;
        mds->mds_osc_obd = ERR_PTR(rc);
        RETURN(rc);
}

int mds_lov_disconnect(struct obd_device *obd, int flags)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;
        ENTRY;

        if (!IS_ERR(mds->mds_osc_obd) && mds->mds_osc_exp != NULL) {
                /* cleanup all llogging subsystems */
                rc = obd_llog_finish(obd, &obd->obd_llogs,
                                     mds->mds_lov_desc.ld_tgt_count);
                if (rc)
                        CERROR("failed to cleanup llogging subsystems\n");

                obd_register_observer(mds->mds_osc_obd, NULL);

                rc = obd_disconnect(mds->mds_osc_exp, flags);
                /* if obd_disconnect fails (probably because the
                 * export was disconnected by class_disconnect_exports)
                 * then we just need to drop our ref. */
                if (rc != 0)
                        class_export_put(mds->mds_osc_exp);
                mds->mds_osc_exp = NULL;
                mds->mds_osc_obd = NULL;
        }

        RETURN(rc);
}

int mds_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                  void *karg, void *uarg)
{
        static struct obd_uuid cfg_uuid = { .uuid = "config_uuid" };
        struct obd_device *obd = exp->exp_obd;
        struct mds_obd *mds = &obd->u.mds;
        struct obd_ioctl_data *data = karg;
        struct lvfs_run_ctxt saved;
        int rc = 0;
        ENTRY;

        switch (cmd) {
        case OBD_IOC_RECORD: {
                char *name = data->ioc_inlbuf1;
                if (mds->mds_cfg_llh)
                        RETURN(-EBUSY);

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = llog_open(llog_get_context(&obd->obd_llogs, 
                                                LLOG_CONFIG_ORIG_CTXT),
                               &mds->mds_cfg_llh, NULL, name,
                               OBD_LLOG_FL_CREATE);
                if (rc == 0)
                        llog_init_handle(mds->mds_cfg_llh, LLOG_F_IS_PLAIN,
                                         &cfg_uuid);
                else
                        mds->mds_cfg_llh = NULL;
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                RETURN(rc);
        }

        case OBD_IOC_ENDRECORD: {
                if (!mds->mds_cfg_llh)
                        RETURN(-EBADF);

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = llog_close(mds->mds_cfg_llh);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                mds->mds_cfg_llh = NULL;
                RETURN(rc);
        }

        case OBD_IOC_CLEAR_LOG: {
                char *name = data->ioc_inlbuf1;
                if (mds->mds_cfg_llh)
                        RETURN(-EBUSY);

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = llog_open(llog_get_context(&obd->obd_llogs, 
                                                LLOG_CONFIG_ORIG_CTXT),
                               &mds->mds_cfg_llh, NULL, name,
                               OBD_LLOG_FL_CREATE);
                if (rc == 0) {
                        llog_init_handle(mds->mds_cfg_llh, LLOG_F_IS_PLAIN,
                                         NULL);

                        rc = llog_destroy(mds->mds_cfg_llh);
                        llog_free_handle(mds->mds_cfg_llh);
                }
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                mds->mds_cfg_llh = NULL;
                RETURN(rc);
        }

        case OBD_IOC_DORECORD: {
                char *cfg_buf;
                struct llog_rec_hdr rec;
                if (!mds->mds_cfg_llh)
                        RETURN(-EBADF);

                /* XXX - this probably should be a parameter to this ioctl.
                 * For now, just use llh_max_transno for expediency. */
                rec.lrh_len = llog_data_len(data->ioc_plen1);

                if (data->ioc_type == LUSTRE_CFG_TYPE) {
                        rec.lrh_type = OBD_CFG_REC;
                } else if (data->ioc_type == PORTALS_CFG_TYPE) {
                        rec.lrh_type = PTL_CFG_REC;
                } else {
                        CERROR("unknown cfg record type:%d \n", data->ioc_type);
                        RETURN(-EINVAL);
                }

                OBD_ALLOC(cfg_buf, data->ioc_plen1);
                if (cfg_buf == NULL)
                        RETURN(-EINVAL);
                rc = copy_from_user(cfg_buf, data->ioc_pbuf1, data->ioc_plen1);
                if (rc) {
                        OBD_FREE(cfg_buf, data->ioc_plen1);
                        RETURN(rc);
                }

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = llog_write_rec(mds->mds_cfg_llh, &rec, NULL, 0,
                                    cfg_buf, -1);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                OBD_FREE(cfg_buf, data->ioc_plen1);
                RETURN(rc);
        }
        case OBD_IOC_SNAP_ADD: {
                char *name = data->ioc_inlbuf1;
                if (name) {
                        rc = fsfilt_set_snap_item(obd, mds->mds_sb, name);
                }
                RETURN(rc);
        }
        case OBD_IOC_PARSE: {
                struct llog_ctxt *ctxt =
                        llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = class_config_process_llog(ctxt, data->ioc_inlbuf1, NULL);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                if (rc)
                        RETURN(rc);

                RETURN(rc);
        }

        case OBD_IOC_DUMP_LOG: {
                struct llog_ctxt *ctxt =
                        llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = class_config_dump_llog(ctxt, data->ioc_inlbuf1, NULL);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                RETURN(rc);
        }

        case OBD_IOC_SET_READONLY: {
                void *handle;
                struct inode *inode = obd->u.mds.mds_sb->s_root->d_inode;
                BDEVNAME_DECLARE_STORAGE(tmp);
                CERROR("setting device %s read-only\n",
                       ll_bdevname(obd->u.mds.mds_sb, tmp));

                handle = fsfilt_start(obd, inode, FSFILT_OP_MKNOD, NULL);
                LASSERT(handle);
                rc = fsfilt_commit(obd, obd->u.mds.mds_sb, inode, handle, 1);

                dev_set_rdonly(ll_sbdev(obd->u.mds.mds_sb), 2);
                RETURN(0);
        }

        case OBD_IOC_CATLOGLIST: {
                int count = mds->mds_lov_desc.ld_tgt_count;
                rc = llog_catalog_list(obd, count, data);
                RETURN(rc);

        }
        case OBD_IOC_LLOG_CHECK:
        case OBD_IOC_LLOG_CANCEL:
        case OBD_IOC_LLOG_REMOVE: {
                struct llog_ctxt *ctxt =
                        llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);
                char name[32] = "CATLIST";
                int rc2;

                obd_llog_finish(obd, &obd->obd_llogs,
                                mds->mds_lov_desc.ld_tgt_count);
                push_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);
                rc = llog_ioctl(ctxt, cmd, data);
                pop_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);
                obd_llog_cat_initialize(obd, &obd->obd_llogs, 
                                        mds->mds_lov_desc.ld_tgt_count,
                                        name);
                rc2 = obd_set_info(mds->mds_osc_exp, strlen("mds_conn"),
                                   "mds_conn", 0, NULL);
                if (!rc)
                        rc = rc2;
                RETURN(rc);
        }
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                struct llog_ctxt *ctxt =
                        llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);

                push_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);
                rc = llog_ioctl(ctxt, cmd, data);
                pop_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);

                RETURN(rc);
        }

        case OBD_IOC_ABORT_RECOVERY:
                CERROR("aborting recovery for device %s\n", obd->obd_name);
                target_abort_recovery(obd);
                RETURN(0);

        default:
                RETURN(-EINVAL);
        }
        RETURN(0);

}

struct mds_lov_sync_info {
        struct obd_device *mlsi_obd; /* the lov device to sync */
        struct obd_uuid   *mlsi_uuid;  /* target to sync */
};

int mds_lov_synchronize(void *data)
{
        struct mds_lov_sync_info *mlsi = data;
        struct llog_ctxt *ctxt;
        struct obd_device *obd;
        struct obd_uuid *uuid;
        unsigned long flags;
        int rc;

        lock_kernel();
        ptlrpc_daemonize();

        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);

        obd = mlsi->mlsi_obd;
        uuid = mlsi->mlsi_uuid;

        OBD_FREE(mlsi, sizeof(*mlsi));

        LASSERT(obd != NULL);
        LASSERT(uuid != NULL);

        rc = obd_set_info(obd->u.mds.mds_osc_exp, strlen("mds_conn"), 
                          "mds_conn", 0, uuid);
        if (rc != 0)
                RETURN(rc);
        
        ctxt = llog_get_context(&obd->obd_llogs, LLOG_UNLINK_ORIG_CTXT);
        LASSERT(ctxt != NULL);

        rc = llog_connect(ctxt, obd->u.mds.mds_lov_desc.ld_tgt_count,
                          NULL, NULL, uuid);
        if (rc != 0) {
                CERROR("%s: failed at llog_origin_connect: %d\n", 
                       obd->obd_name, rc);
                RETURN(rc);
        }
        
        CWARN("MDS %s: %s now active, resetting orphans\n",
              obd->obd_name, uuid->uuid);
        rc = mds_lov_clearorphans(&obd->u.mds, uuid);
        if (rc != 0) {
                CERROR("%s: failed at mds_lov_clearorphans: %d\n", 
                       obd->obd_name, rc);
                RETURN(rc);
        }

        RETURN(0);
}

int mds_lov_start_synchronize(struct obd_device *obd, struct obd_uuid *uuid)
{
        struct mds_lov_sync_info *mlsi;
        int rc;
        
        ENTRY;

        OBD_ALLOC(mlsi, sizeof(*mlsi));
        if (mlsi == NULL)
                RETURN(-ENOMEM);

        mlsi->mlsi_obd = obd;
        mlsi->mlsi_uuid = uuid;

        rc = kernel_thread(mds_lov_synchronize, mlsi, CLONE_VM | CLONE_FILES);
        if (rc < 0)
                CERROR("%s: error starting mds_lov_synchronize: %d\n", 
                       obd->obd_name, rc);
        else {
                CDEBUG(D_HA, "%s: mds_lov_synchronize thread: %d\n", 
                       obd->obd_name, rc);
                rc = 0;
        }

        RETURN(rc);
}

int mds_notify(struct obd_device *obd, struct obd_device *watched, int active)
{
        struct obd_uuid *uuid;
        int rc = 0;
        ENTRY;

        if (!active)
                RETURN(0);

        if (strcmp(watched->obd_type->typ_name, LUSTRE_OSC_NAME)) {
                CERROR("unexpected notification of %s %s!\n",
                       watched->obd_type->typ_name, watched->obd_name);
                RETURN(-EINVAL);
        }

        uuid = &watched->u.cli.cl_import->imp_target_uuid;
        if (obd->obd_recovering) {
                CWARN("MDS %s: in recovery, not resetting orphans on %s\n",
                      obd->obd_name, uuid->uuid);
        } else {
                rc = mds_lov_start_synchronize(obd, uuid);
        }
        RETURN(rc);
}

int mds_set_info(struct obd_export *exp, obd_count keylen,
                 void *key, obd_count vallen, void *val)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct mds_obd *mds = &obd->u.mds;
        ENTRY;

#define KEY_IS(str) \
        (keylen == strlen(str) && memcmp(key, str, keylen) == 0)

        if (KEY_IS("next_id")) {
                obd_id *id = (obd_id *)val;
                int idx, rc;

                /* XXX - this really should be vallen != (2 * sizeof(*id)) *
                 * Just following the precedent set by lov_set_info.       */
                if (vallen != 2)
                        RETURN(-EINVAL);

                if ((idx = *id) != *id)
                        RETURN(-EINVAL);

                CDEBUG(D_CONFIG, "idx: %d id: %llu\n", idx, *(id + 1));

                /* The size of the LOV target table may have increased. */
                if (idx >= mds->mds_lov_desc.ld_tgt_count) {
                        obd_id *ids;
                        int oldsize, size;

                        oldsize = mds->mds_lov_desc.ld_tgt_count * sizeof(*ids);
                        rc = mds_lov_update_desc(obd, mds->mds_osc_exp);
                        if (rc)
                                RETURN(rc);
                        if (idx >= mds->mds_lov_desc.ld_tgt_count)
                                RETURN(-EINVAL);
                        size = mds->mds_lov_desc.ld_tgt_count * sizeof(*ids);
                        OBD_ALLOC(ids, size);
                        if (ids == NULL)
                                RETURN(-ENOMEM);
                        memset(ids, 0, size);
                        if (mds->mds_lov_objids != NULL) {
                                memcpy(ids, mds->mds_lov_objids, oldsize);
                                OBD_FREE(mds->mds_lov_objids, oldsize);
                        }
                        mds->mds_lov_objids = ids;
                }

                mds->mds_lov_objids[idx] = *++id;
                CDEBUG(D_CONFIG, "objid: %d: %lld\n", idx, *id);
                /* XXX - should we be writing this out here ? */
                RETURN(mds_lov_write_objids(obd));
        }

        RETURN(-EINVAL);
}

int mds_lov_update_config(struct obd_device *obd, int clean)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lvfs_run_ctxt saved;
        struct config_llog_instance cfg;
        struct llog_ctxt *ctxt;
        char *profile = mds->mds_profile, *name;
        int rc, version, namelen;
        ENTRY;

        if (profile == NULL)
                RETURN(0);

        cfg.cfg_instance = NULL;
        cfg.cfg_uuid = mds->mds_lov_uuid;

        namelen = strlen(profile) + 20; /* -clean-######### */
        OBD_ALLOC(name, namelen);
        if (name == NULL)
                RETURN(-ENOMEM);

        if (clean) {
                version = mds->mds_config_version - 1;
                sprintf(name, "%s-clean-%d", profile, version);
        } else {
                version = mds->mds_config_version + 1;
                sprintf(name, "%s-%d", profile, version);
        }

        CWARN("Applying configuration log %s\n", name);

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        ctxt = llog_get_context(&obd->obd_llogs, LLOG_CONFIG_ORIG_CTXT);
        rc = class_config_process_llog(ctxt, name, &cfg);
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (rc == 0) {
                mds->mds_config_version = version;
                rc = mds_lov_update_desc(obd, mds->mds_osc_exp);
        }
        CWARN("Finished applying configuration log %s: %d\n", name, rc);

        OBD_FREE(name, namelen);
        RETURN(rc);
}

/* Convert the on-disk LOV EA structre.
 * We always try to convert from an old LOV EA format to the common in-memory
 * (lsm) format (obd_unpackmd() understands the old on-disk (lmm) format) and
 * then convert back to the new on-disk format and save it back to disk
 * (obd_packmd() only ever saves to the new on-disk format) so we don't have
 * to convert it each time this inode is accessed.
 *
 * This function is a bit interesting in the error handling.  We can safely
 * ship the old lmm to the client in case of failure, since it uses the same
 * obd_unpackmd() code and can do the conversion if the MDS fails for some
 * reason.  We will not delete the old lmm data until we have written the
 * new format lmm data in fsfilt_set_md(). */
int mds_convert_lov_ea(struct obd_device *obd, struct inode *inode,
                       struct lov_mds_md *lmm, int lmm_size)
{
        struct lov_stripe_md *lsm = NULL;
        void *handle;
        int rc, err;
        ENTRY;

        if (le32_to_cpu(lmm->lmm_magic) == LOV_MAGIC)
                RETURN(0);

        CWARN("converting LOV EA on %lu/%u from V0 to V1\n",
              inode->i_ino, inode->i_generation);
        rc = obd_unpackmd(obd->u.mds.mds_osc_exp, &lsm, lmm, lmm_size);
        if (rc < 0)
                GOTO(conv_end, rc);

        rc = obd_packmd(obd->u.mds.mds_osc_exp, &lmm, lsm);
        if (rc < 0)
                GOTO(conv_free, rc);
        lmm_size = rc;

        handle = fsfilt_start(obd, inode, FSFILT_OP_SETATTR, NULL);
        if (IS_ERR(handle)) {
                rc = PTR_ERR(handle);
                GOTO(conv_free, rc);
        }

        rc = fsfilt_set_md(obd, inode, handle, lmm, lmm_size);

        err = fsfilt_commit(obd, obd->u.mds.mds_sb, inode, handle, 0);
        if (!rc)
                rc = err ? err : lmm_size;
        GOTO(conv_free, rc);
conv_free:
        obd_free_memmd(obd->u.mds.mds_osc_exp, &lsm);
conv_end:
        return rc;
}

/* Must be called with i_sem held */
int mds_revalidate_lov_ea(struct obd_device *obd, struct inode *inode,
                          struct lustre_msg *msg, int offset)
{
        struct mds_obd *mds = &obd->u.mds;
        struct obd_export *osc_exp = mds->mds_osc_exp;
        struct lov_mds_md *lmm= NULL;
        struct lov_stripe_md *lsm = NULL;
        struct obdo *oa;
        struct obd_trans_info oti = {0};
        unsigned valid = 0;
        int lmm_size = 0, lsm_size = 0, err, rc;
        void *handle;
        ENTRY;

        LASSERT(down_trylock(&inode->i_sem) != 0);

        /* XXX - add way to know if EA is already up to date & return
         * without doing anything. Easy to do since we get notified of
         * LOV updates. */

        lmm = lustre_msg_buf(msg, offset, 0);
        if (lmm == NULL) {
                CDEBUG(D_INFO, "no space reserved for inode %lu MD\n",
                       inode->i_ino);
                RETURN(0);
        }
        lmm_size = msg->buflens[offset];

        rc = obd_unpackmd(osc_exp, &lsm, lmm, lmm_size);
        if (rc < 0)
                RETURN(0);

        lsm_size = rc;

        LASSERT(lsm->lsm_magic == LOV_MAGIC);

        oa = obdo_alloc();
        if (oa == NULL)
                GOTO(out_lsm, rc = -ENOMEM);
        oa->o_mode = S_IFREG | 0600;
        oa->o_id = inode->i_ino;
        oa->o_generation = inode->i_generation;
        oa->o_uid = 0; /* must have 0 uid / gid on OST */
        oa->o_gid = 0;

        oa->o_valid = OBD_MD_FLID | OBD_MD_FLGENER | OBD_MD_FLTYPE |
                      OBD_MD_FLMODE | OBD_MD_FLUID | OBD_MD_FLGID;
        valid = OBD_MD_FLTYPE | OBD_MD_FLATIME | OBD_MD_FLMTIME |
                OBD_MD_FLCTIME;
        obdo_from_inode(oa, inode, valid);

        rc = obd_revalidate_md(osc_exp, oa, lsm, &oti);
        if (rc == 0)
                GOTO(out_oa, rc);
        if (rc < 0) {
                CERROR("Error validating LOV EA on %lu/%u: %d\n",
                       inode->i_ino, inode->i_generation, rc);
                GOTO(out_oa, rc);
        }

        rc = obd_packmd(osc_exp, &lmm, lsm);
        if (rc < 0)
                GOTO(out_oa, rc);
        lmm_size = rc;

        handle = fsfilt_start(obd, inode, FSFILT_OP_SETATTR, NULL);
        if (IS_ERR(handle)) {
                rc = PTR_ERR(handle);
                GOTO(out_oa, rc);
        }

        rc = fsfilt_set_md(obd, inode, handle, lmm, lmm_size);
        err = fsfilt_commit(obd, inode->i_sb, inode, handle, 0);
        if (!rc)
                rc = err;
out_oa:
        obdo_free(oa);
out_lsm:
        obd_free_memmd(osc_exp, &lsm);
        RETURN(rc);
}
