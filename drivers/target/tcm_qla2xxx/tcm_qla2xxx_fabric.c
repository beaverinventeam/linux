/*******************************************************************************
 * This file contains TCM_QLA2XXX functions for struct target_core_fabrib_ops
 * for Qlogic 2xxx series target mode HBAs
 *
 * © Copyright 2010-2011 RisingTide Systems LLC.
 *
 * Licensed to the Linux Foundation under the General Public License (GPL) version 2.
 *
 * Author: Nicholas A. Bellinger <nab@risingtidesystems.com>
 *
 * tcm_qla2xxx_parse_wwn() and tcm_qla2xxx_format_wwn() contains code from
 * the TCM_FC / Open-FCoE.org fabric module.
 *
 * Copyright (c) 2010 Cisco Systems, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/

#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <asm/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>

#include <target/target_core_base.h>
#include <target/target_core_transport.h>
#include <target/target_core_fabric_ops.h>
#include <target/target_core_fabric_lib.h>
#include <target/target_core_device.h>
#include <target/target_core_tpg.h>
#include <target/target_core_configfs.h>
#include <target/target_core_tmr.h>

#include <qla_def.h>
#include <qla_target.h>

#include "tcm_qla2xxx_base.h"
#include "tcm_qla2xxx_fabric.h"

int tcm_qla2xxx_check_true(struct se_portal_group *se_tpg)
{
	return 1;
}

int tcm_qla2xxx_check_false(struct se_portal_group *se_tpg)
{
	return 0;
}

/*
 * Parse WWN.
 * If strict, we require lower-case hex and colon separators to be sure
 * the name is the same as what would be generated by ft_format_wwn()
 * so the name and wwn are mapped one-to-one.
 */
ssize_t tcm_qla2xxx_parse_wwn(const char *name, u64 *wwn, int strict)
{
	const char *cp;
	char c;
	u32 nibble;
	u32 byte = 0;
	u32 pos = 0;
	u32 err;

	*wwn = 0;
	for (cp = name; cp < &name[TCM_QLA2XXX_NAMELEN - 1]; cp++) {
		c = *cp;
		if (c == '\n' && cp[1] == '\0')
			continue;
		if (strict && pos++ == 2 && byte++ < 7) {
			pos = 0;
			if (c == ':')
				continue;
			err = 1;
			goto fail;
		}
		if (c == '\0') {
			err = 2;
			if (strict && byte != 8)
				goto fail;
			return cp - name;
		}
		err = 3;
		if (isdigit(c))
			nibble = c - '0';
		else if (isxdigit(c) && (islower(c) || !strict))
			nibble = tolower(c) - 'a' + 10;
		else
			goto fail;
		*wwn = (*wwn << 4) | nibble;
	}
	err = 4;
fail:
	printk(KERN_INFO "err %u len %zu pos %u byte %u\n",
			err, cp - name, pos, byte);
	return -1;
}

ssize_t tcm_qla2xxx_format_wwn(char *buf, size_t len, u64 wwn)
{
	u8 b[8];

	put_unaligned_be64(wwn, b);
	return snprintf(buf, len,
		"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

char *tcm_qla2xxx_get_fabric_name(void)
{
	return "qla2xxx";
}

/*
 * From drivers/scsi/scsi_transport_fc.c:fc_parse_wwn
 */
static int tcm_qla2xxx_npiv_extract_wwn(const char *ns, u64 *nm)
{
	unsigned int i, j, value;
	u8 wwn[8];

	memset(wwn, 0, sizeof(wwn));

	/* Validate and store the new name */
	for (i = 0, j = 0; i < 16; i++) {
		value = hex_to_bin(*ns++);
		if (value >= 0)
			j = (j << 4) | value;
		else
			return -EINVAL;

		if (i % 2) {
			wwn[i/2] = j & 0xff;
			j = 0;
		}
	}

	*nm = wwn_to_u64(wwn);
	return 0;
}

/*
 * This parsing logic follows drivers/scsi/scsi_transport_fc.c:store_fc_host_vport_create()
 */
int tcm_qla2xxx_npiv_parse_wwn(
	const char *name,
	size_t count,
	u64 *wwpn,
	u64 *wwnn)
{
	unsigned int cnt = count;
	int rc;

	*wwpn = 0;
	*wwnn = 0;

	/* count may include a LF at end of string */
	if (name[cnt-1] == '\n')
		cnt--;

	/* validate we have enough characters for WWPN */
	if ((cnt != (16+1+16)) || (name[16] != ':'))
		return -EINVAL;

	rc = tcm_qla2xxx_npiv_extract_wwn(&name[0], wwpn);
	if (rc != 0)
		return rc;

	rc = tcm_qla2xxx_npiv_extract_wwn(&name[17], wwnn);
	if (rc != 0)
		return rc;

	return 0;
}

ssize_t tcm_qla2xxx_npiv_format_wwn(char *buf, size_t len, u64 wwpn, u64 wwnn)
{
	u8 b[8], b2[8];

	put_unaligned_be64(wwpn, b);
	put_unaligned_be64(wwnn, b2);
        return snprintf(buf, len,
                "%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x,"
		"%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		b2[0], b2[1], b2[2], b2[3], b2[4], b2[5], b2[6], b2[7]);
}

char *tcm_qla2xxx_npiv_get_fabric_name(void)
{
	return "qla2xxx_npiv";
}

u8 tcm_qla2xxx_get_fabric_proto_ident(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;
	u8 proto_id;

	switch (lport->lport_proto_id) {
	case SCSI_PROTOCOL_FCP:
	default:
		proto_id = fc_get_fabric_proto_ident(se_tpg);
		break;
	}

	return proto_id;
}

char *tcm_qla2xxx_get_fabric_wwn(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;

	return &lport->lport_name[0];
}

char *tcm_qla2xxx_npiv_get_fabric_wwn(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;

	return &lport->lport_npiv_name[0];
}

u16 tcm_qla2xxx_get_tag(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	return tpg->lport_tpgt;
}

u32 tcm_qla2xxx_get_default_depth(struct se_portal_group *se_tpg)
{
	return 1;
}

u32 tcm_qla2xxx_get_pr_transport_id(
	struct se_portal_group *se_tpg,
	struct se_node_acl *se_nacl,
	struct t10_pr_registration *pr_reg,
	int *format_code,
	unsigned char *buf)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;
	int ret = 0;

	switch (lport->lport_proto_id) {
	case SCSI_PROTOCOL_FCP:
	default:
		ret = fc_get_pr_transport_id(se_tpg, se_nacl, pr_reg,
					format_code, buf);
		break;
	}

	return ret;
}		

u32 tcm_qla2xxx_get_pr_transport_id_len(
	struct se_portal_group *se_tpg,
	struct se_node_acl *se_nacl,
	struct t10_pr_registration *pr_reg,
	int *format_code)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;
	int ret = 0;

	switch (lport->lport_proto_id) {
	case SCSI_PROTOCOL_FCP:
	default:
		ret = fc_get_pr_transport_id_len(se_tpg, se_nacl, pr_reg,
					format_code);
		break;
	}

	return ret;
}

char *tcm_qla2xxx_parse_pr_out_transport_id(
	struct se_portal_group *se_tpg,
	const char *buf,
	u32 *out_tid_len,
	char **port_nexus_ptr)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);
	struct tcm_qla2xxx_lport *lport = tpg->lport;
	char *tid = NULL;

	switch (lport->lport_proto_id) {
	case SCSI_PROTOCOL_FCP:
	default:
		tid = fc_parse_pr_out_transport_id(se_tpg, buf, out_tid_len,
					port_nexus_ptr);
		break;
	}

	return tid;
}

int tcm_qla2xxx_check_demo_mode(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);

	return QLA_TPG_ATTRIB(tpg)->generate_node_acls;
}

int tcm_qla2xxx_check_demo_mode_cache(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);

	return QLA_TPG_ATTRIB(tpg)->cache_dynamic_acls;
}

int tcm_qla2xxx_check_demo_write_protect(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);

	return QLA_TPG_ATTRIB(tpg)->demo_mode_write_protect;
}

int tcm_qla2xxx_check_prod_write_protect(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);

	return QLA_TPG_ATTRIB(tpg)->prod_mode_write_protect;
}

struct se_node_acl *tcm_qla2xxx_alloc_fabric_acl(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_nacl *nacl;

	nacl = kzalloc(sizeof(struct tcm_qla2xxx_nacl), GFP_KERNEL);
	if (!(nacl)) {
		printk(KERN_ERR "Unable to alocate struct tcm_qla2xxx_nacl\n");
		return NULL;
	}

	return &nacl->se_node_acl;
}

void tcm_qla2xxx_release_fabric_acl(
	struct se_portal_group *se_tpg,
	struct se_node_acl *se_nacl)
{
	struct tcm_qla2xxx_nacl *nacl = container_of(se_nacl,
			struct tcm_qla2xxx_nacl, se_node_acl);
	kfree(nacl);
}

u32 tcm_qla2xxx_tpg_get_inst_index(struct se_portal_group *se_tpg)
{
	struct tcm_qla2xxx_tpg *tpg = container_of(se_tpg,
				struct tcm_qla2xxx_tpg, se_tpg);

	return tpg->lport_tpgt;
}

/*
 * Called from qla_target_template->free_cmd(), and will call
 * tcm_qla2xxx_release_cmd via normal struct target_core_fabric_ops
 * release callback.
 */
void tcm_qla2xxx_free_cmd(struct qla_tgt_cmd *cmd)
{
	barrier();
	transport_generic_free_cmd_intr(&cmd->se_cmd);
}

/*
 * Called from struct target_core_fabric_ops->check_stop_free() context
 */
void tcm_qla2xxx_check_stop_free(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);
	struct qla_tgt_mgmt_cmd *mcmd;

	if (se_cmd->se_tmr_req) {
		mcmd = container_of(se_cmd, struct qla_tgt_mgmt_cmd, se_cmd);
		/*
		 * Release the associated se_cmd->se_tmr_req and se_cmd
		 * TMR related state now.
		 */
		transport_generic_free_cmd(se_cmd, 1, 0);
		qla_tgt_free_mcmd(mcmd);
		return;
	}

	atomic_set(&cmd->cmd_stop_free, 1);
	barrier();
}

/*
 * Callback from TCM Core to release underlying fabric descriptor
 */
void tcm_qla2xxx_release_cmd(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);

	if (se_cmd->se_tmr_req != NULL)
		return;

	while (atomic_read(&cmd->cmd_stop_free) != 1) {
		printk(KERN_WARNING "Hit atomic_read(&cmd->cmd_stop_free)=1"
				" in tcm_qla2xxx_release_cmd\n");
		cpu_relax();
	}

	qla_tgt_free_cmd(cmd);
}

int tcm_qla2xxx_shutdown_session(struct se_session *se_sess)
{
	struct qla_tgt_sess *sess = se_sess->fabric_sess_ptr;

	if (!sess) {
		printk("se_sess->fabric_sess_ptr is NULL\n");
		dump_stack();
		return 0;
	}
	return 1;
}

extern int tcm_qla2xxx_clear_nacl_from_fcport_map(struct se_node_acl *);

void tcm_qla2xxx_close_session(struct se_session *se_sess)
{
	struct se_node_acl *se_nacl = se_sess->se_node_acl;
	struct qla_tgt_sess *sess = se_sess->fabric_sess_ptr;
	struct scsi_qla_host *vha;
	unsigned long flags;

	if (!sess) {
		printk(KERN_ERR "se_sess->fabric_sess_ptr is NULL\n");
		dump_stack();
		return;
	}
	vha = sess->vha;

	spin_lock_irqsave(&vha->hw->hardware_lock, flags);
	tcm_qla2xxx_clear_nacl_from_fcport_map(se_nacl);
	qla_tgt_sess_put(sess);
	spin_unlock_irqrestore(&vha->hw->hardware_lock, flags);
}

void tcm_qla2xxx_stop_session(struct se_session *se_sess, int sess_sleep , int conn_sleep)
{
	struct qla_tgt_sess *sess = se_sess->fabric_sess_ptr;
	struct scsi_qla_host *vha;
	unsigned long flags;

	if (!sess) {
		printk(KERN_ERR "se_sess->fabric_sess_ptr is NULL\n");
		dump_stack();
		return;
	}
	vha = sess->vha;

	spin_lock_irqsave(&vha->hw->hardware_lock, flags);
	tcm_qla2xxx_clear_nacl_from_fcport_map(se_sess->se_node_acl);
	spin_unlock_irqrestore(&vha->hw->hardware_lock, flags);
}

void tcm_qla2xxx_reset_nexus(struct se_session *se_sess)
{
	return;
}

int tcm_qla2xxx_sess_logged_in(struct se_session *se_sess)
{
	return 0;
}

u32 tcm_qla2xxx_sess_get_index(struct se_session *se_sess)
{
	return 0;
}

int tcm_qla2xxx_write_pending(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);

	cmd->bufflen = se_cmd->data_length;
	cmd->dma_data_direction = se_cmd->data_direction;
	/*
	 * Setup the struct se_task->task_sg[] chained SG list
	 */
	if ((se_cmd->se_cmd_flags & SCF_SCSI_DATA_SG_IO_CDB) ||
	    (se_cmd->se_cmd_flags & SCF_SCSI_CONTROL_SG_IO_CDB)) {
		transport_do_task_sg_chain(se_cmd);

		cmd->sg_cnt = se_cmd->t_tasks_sg_chained_no;
		cmd->sg = se_cmd->t_tasks_sg_chained;
	} else if (se_cmd->se_cmd_flags & SCF_SCSI_CONTROL_NONSG_IO_CDB) {
		/*
		 * Use se_cmd->t_task->t_tasks_sg_bounce for control CDBs
		 * using a contiguous buffer
		 */
		sg_init_table(&se_cmd->t_tasks_sg_bounce, 1);
		sg_set_buf(&se_cmd->t_tasks_sg_bounce,
			se_cmd->t_task_buf, se_cmd->data_length);
		cmd->sg_cnt = 1;
		cmd->sg = &se_cmd->t_tasks_sg_bounce;
	} else {
		printk(KERN_ERR "Unknown se_cmd_flags: 0x%08x in"
			" tcm_qla2xxx_write_pending()\n", se_cmd->se_cmd_flags);
		BUG();
	}
	/*
	 * qla_target.c:qla_tgt_rdy_to_xfer() will call pci_map_sg() to setup
	 * the SGL mappings into PCIe memory for incoming FCP WRITE data.
	 */
	return qla_tgt_rdy_to_xfer(cmd);
}

int tcm_qla2xxx_write_pending_status(struct se_cmd *se_cmd)
{
	return 0;
}

void tcm_qla2xxx_set_default_node_attrs(struct se_node_acl *nacl)
{
	return;
}

u32 tcm_qla2xxx_get_task_tag(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);

	return cmd->tag;
}

int tcm_qla2xxx_get_cmd_state(struct se_cmd *se_cmd)
{
	return 0;
}

/*
 * Main entry point for incoming ATIO packets from qla_target.c
 * and qla2xxx LLD code.
 */
int tcm_qla2xxx_handle_cmd(scsi_qla_host_t *vha, struct qla_tgt_cmd *cmd,
			uint32_t lun, uint32_t data_length,
			int fcp_task_attr, int data_dir, int bidi)
{
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct se_session *se_sess;
	struct se_portal_group *se_tpg;
	struct qla_tgt_sess *sess;

	sess = cmd->sess;
	if (!sess) {
		printk(KERN_ERR "Unable to locate struct qla_tgt_sess from qla_tgt_cmd\n");
		return -EINVAL;
	}

	se_sess = sess->se_sess;
	if (!se_sess) {
		printk(KERN_ERR "Unable to locate active struct se_session\n");
		return -EINVAL;
	}
	se_tpg = se_sess->se_tpg;

	/*
	 * Initialize struct se_cmd descriptor from target_core_mod infrastructure
	 */
	transport_init_se_cmd(se_cmd, se_tpg->se_tpg_tfo, se_sess,
			data_length, data_dir,
			fcp_task_attr, &cmd->sense_buffer[0]);
	/*
	 * Signal BIDI usage with T_TASK(cmd)->t_tasks_bidi
	 */
	if (bidi)
		se_cmd->t_tasks_bidi = 1;
	/*
	 * Locate the struct se_lun pointer and attach it to struct se_cmd
	 */
	if (transport_lookup_cmd_lun(se_cmd, lun) < 0) {
		/*
		 * Clear qla_tgt_cmd->locked_rsp as ha->hardware_lock
		 * is already held here..
		 */
		if (spin_is_locked(&cmd->vha->hw->hardware_lock))
			cmd->locked_rsp = 0;

		/* NON_EXISTENT_LUN */
		transport_send_check_condition_and_sense(se_cmd,
				se_cmd->scsi_sense_reason, 0);
		return 0;
	}
	/*
	 * Queue up the newly allocated to be processed in TCM thread context.
	 */
	transport_generic_handle_cdb_map(se_cmd);
	return 0;
}

int tcm_qla2xxx_new_cmd_map(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);
	scsi_qla_host_t *vha = cmd->vha;
	struct qla_hw_data *ha = vha->hw;
	unsigned char *cdb;
	int ret;

	if (IS_FWI2_CAPABLE(ha)) {
		atio7_entry_t *atio = &cmd->atio.atio7;
		cdb = &atio->fcp_cmnd.cdb[0];
	} else {
		atio_entry_t *atio = &cmd->atio.atio2x;
		cdb = &atio->cdb[0];
	}

	/*
	 * Allocate the necessary tasks to complete the received CDB+data
	 */
	ret = transport_generic_allocate_tasks(se_cmd, cdb);
	if (ret == -ENOMEM) {
		/* Out of Resources */
		return PYX_TRANSPORT_OUT_OF_MEMORY_RESOURCES;
	} else if (ret == -EINVAL) {
		/*
		 * Handle case for SAM_STAT_RESERVATION_CONFLICT
		 */
		if (se_cmd->se_cmd_flags & SCF_SCSI_RESERVATION_CONFLICT)
			return PYX_TRANSPORT_RESERVATION_CONFLICT;
		/*
		 * Otherwise, se_cmd->scsi_sense_reason will be set, so
		 * return PYX_TRANSPORT_USE_SENSE_REASON to signal
		 * transport_generic_request_failure()
		 */
		return PYX_TRANSPORT_USE_SENSE_REASON;
	}
	/*
	 * drivers/target/target_core_transport.c:transport_processing_thread()
	 * falls through to TRANSPORT_NEW_CMD.
	 */
	return 0;
}

/*
 * Called from qla_target.c:qla_tgt_do_ctio_completion()
 */
int tcm_qla2xxx_handle_data(struct qla_tgt_cmd *cmd)
{
	/*
	 * We now tell TCM to queue this WRITE CDB with TRANSPORT_PROCESS_WRITE
	 * status to the backstore processing thread.
	 */
	return transport_generic_handle_data(&cmd->se_cmd);
}

/*
 * Called from qla_target.c:qla_tgt_issue_task_mgmt()
 */
int tcm_qla2xxx_handle_tmr(struct qla_tgt_mgmt_cmd *mcmd, uint32_t lun, uint8_t tmr_func)
{
	struct qla_tgt_sess *sess = mcmd->sess;
	struct se_session *se_sess = sess->se_sess;
	struct se_portal_group *se_tpg = se_sess->se_tpg;
	struct se_cmd *se_cmd = &mcmd->se_cmd;
	/*
	 * Initialize struct se_cmd descriptor from target_core_mod infrastructure
	 */
	transport_init_se_cmd(se_cmd, se_tpg->se_tpg_tfo, se_sess, 0,
				DMA_NONE, 0, NULL);
	/*
	 * Allocate the TCM TMR
	 */
	se_cmd->se_tmr_req = core_tmr_alloc_req(se_cmd, (void *)mcmd, tmr_func);
	if (!se_cmd->se_tmr_req)
		return -ENOMEM;
	/*
	 * Save the se_tmr_req for qla_tgt_xmit_tm_rsp() callback into LLD code
	 */
	mcmd->se_tmr_req = se_cmd->se_tmr_req;
	/*
	 * Locate the underlying TCM struct se_lun from sc->device->lun
	 */
	if (transport_lookup_tmr_lun(se_cmd, lun) < 0) {
		transport_generic_free_cmd(se_cmd, 1, 0);
		return -EINVAL;
	}
	/*
	 * Queue the TMR associated se_cmd into TCM Core for processing
	 */
	return transport_generic_handle_tmr(se_cmd);
}

int tcm_qla2xxx_queue_data_in(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);

	cmd->bufflen = se_cmd->data_length;
	cmd->dma_data_direction = se_cmd->data_direction;
	cmd->aborted = atomic_read(&se_cmd->t_transport_aborted);
	/*
	 * Setup the struct se_task->task_sg[] chained SG list
	 */
	if ((se_cmd->se_cmd_flags & SCF_SCSI_DATA_SG_IO_CDB) ||
	    (se_cmd->se_cmd_flags & SCF_SCSI_CONTROL_SG_IO_CDB)) {
		transport_do_task_sg_chain(se_cmd);

		cmd->sg_cnt = se_cmd->t_tasks_sg_chained_no;
		cmd->sg = se_cmd->t_tasks_sg_chained;
	} else if (se_cmd->se_cmd_flags & SCF_SCSI_CONTROL_NONSG_IO_CDB) {
		/*
		 * Use se_cmd->t_task->t_tasks_sg_bounce for control CDBs
		 * using a contigious buffer
		 */
		sg_init_table(&se_cmd->t_tasks_sg_bounce, 1);
		sg_set_buf(&se_cmd->t_tasks_sg_bounce,
			se_cmd->t_task_buf, se_cmd->data_length);

		cmd->sg_cnt = 1;
		cmd->sg = &se_cmd->t_tasks_sg_bounce;
	} else {
		cmd->sg_cnt = 0;
		cmd->sg = NULL;
	}

	cmd->offset = 0;

	/*
	 * Now queue completed DATA_IN the qla2xxx LLD and response ring
	 */
	return qla2xxx_xmit_response(cmd, QLA_TGT_XMIT_DATA|QLA_TGT_XMIT_STATUS,
				se_cmd->scsi_status);
}

int tcm_qla2xxx_queue_status(struct se_cmd *se_cmd)
{
	struct qla_tgt_cmd *cmd = container_of(se_cmd, struct qla_tgt_cmd, se_cmd);

	cmd->bufflen = se_cmd->data_length;
	cmd->sg = NULL;
	cmd->sg_cnt = 0;
	cmd->offset = 0;
	cmd->dma_data_direction = se_cmd->data_direction;
	cmd->aborted = atomic_read(&se_cmd->t_transport_aborted);

	/*
	 * Now queue status response to qla2xxx LLD code and response ring
	 */
	return qla2xxx_xmit_response(cmd, QLA_TGT_XMIT_STATUS, se_cmd->scsi_status);
}

int tcm_qla2xxx_queue_tm_rsp(struct se_cmd *se_cmd)
{
	struct se_tmr_req *se_tmr = se_cmd->se_tmr_req;
	struct qla_tgt_mgmt_cmd *mcmd = container_of(se_cmd,
				struct qla_tgt_mgmt_cmd, se_cmd);

	printk("queue_tm_rsp: mcmd: %p func: 0x%02x response: 0x%02x\n",
			mcmd, se_tmr->function, se_tmr->response);
	/*
	 * Do translation between TCM TM response codes and
	 * QLA2xxx FC TM response codes.
	 */
	switch (se_tmr->response) {
	case TMR_FUNCTION_COMPLETE:
		mcmd->fc_tm_rsp = FC_TM_SUCCESS;
		break;
	case TMR_TASK_DOES_NOT_EXIST:
		mcmd->fc_tm_rsp = FC_TM_BAD_CMD;
		break;
	case TMR_FUNCTION_REJECTED:
		mcmd->fc_tm_rsp = FC_TM_REJECT;
		break;
	case TMR_LUN_DOES_NOT_EXIST:
	default:
		mcmd->fc_tm_rsp = FC_TM_FAILED;
		break;
	}
	/*
	 * Queue the TM response to QLA2xxx LLD to build a
	 * CTIO response packet.
	 */
	qla_tgt_xmit_tm_rsp(mcmd);

	return 0;
}

u16 tcm_qla2xxx_get_fabric_sense_len(void)
{
	return 0;
}

u16 tcm_qla2xxx_set_fabric_sense_len(struct se_cmd *se_cmd, u32 sense_length)
{
	return 0;
}

int tcm_qla2xxx_is_state_remove(struct se_cmd *se_cmd)
{
	return 0;
}