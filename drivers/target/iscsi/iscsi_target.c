/*******************************************************************************
 * This file contains main functions related to the iSCSI Target Core Driver.
 *
 * © Copyright 2007-2011 RisingTide Systems LLC.
 *
 * Licensed to the Linux Foundation under the General Public License (GPL) version 2.
 *
 * Author: Nicholas A. Bellinger <nab@linux-iscsi.org>
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
 ******************************************************************************/

#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/crypto.h>
#include <linux/completion.h>
#include <asm/unaligned.h>
#include <scsi/scsi_device.h>
#include <scsi/iscsi_proto.h>
#include <target/target_core_base.h>
#include <target/target_core_tmr.h>
#include <target/target_core_transport.h>

#include "iscsi_target_debug.h"
#include "iscsi_target_core.h"
#include "iscsi_target_parameters.h"
#include "iscsi_target_seq_pdu_list.h"
#include "iscsi_target_tq.h"
#include "iscsi_target_configfs.h"
#include "iscsi_target_datain_values.h"
#include "iscsi_target_erl0.h"
#include "iscsi_target_erl1.h"
#include "iscsi_target_erl2.h"
#include "iscsi_target_login.h"
#include "iscsi_target_tmr.h"
#include "iscsi_target_tpg.h"
#include "iscsi_target_util.h"
#include "iscsi_target.h"
#include "iscsi_target_device.h"
#include "iscsi_target_stat.h"

static LIST_HEAD(g_tiqn_list);
static LIST_HEAD(g_np_list);
static DEFINE_SPINLOCK(tiqn_lock);
static DEFINE_SPINLOCK(np_lock);

static struct idr tiqn_idr;
struct idr sess_idr;
struct mutex auth_id_lock;
spinlock_t sess_idr_lock;

struct iscsit_global *iscsit_global;

struct kmem_cache *lio_cmd_cache;
struct kmem_cache *lio_qr_cache;
struct kmem_cache *lio_dr_cache;
struct kmem_cache *lio_ooo_cache;
struct kmem_cache *lio_r2t_cache;

static int iscsit_handle_immediate_data(struct iscsi_cmd *,
			unsigned char *buf, u32);
static int iscsit_logout_post_handler(struct iscsi_cmd *, struct iscsi_conn *);

struct iscsi_tiqn *iscsit_get_tiqn_for_login(unsigned char *buf)
{
	struct iscsi_tiqn *tiqn = NULL;

	spin_lock(&tiqn_lock);
	list_for_each_entry(tiqn, &g_tiqn_list, tiqn_list) {
		if (!strcmp(tiqn->tiqn, buf)) {

			spin_lock(&tiqn->tiqn_state_lock);
			if (tiqn->tiqn_state == TIQN_STATE_ACTIVE) {
				tiqn->tiqn_access_count++;
				spin_unlock(&tiqn->tiqn_state_lock);
				spin_unlock(&tiqn_lock);
				return tiqn;
			}
			spin_unlock(&tiqn->tiqn_state_lock);
		}
	}
	spin_unlock(&tiqn_lock);

	return NULL;
}

static int iscsit_set_tiqn_shutdown(struct iscsi_tiqn *tiqn)
{
	spin_lock(&tiqn->tiqn_state_lock);
	if (tiqn->tiqn_state == TIQN_STATE_ACTIVE) {
		tiqn->tiqn_state = TIQN_STATE_SHUTDOWN;
		spin_unlock(&tiqn->tiqn_state_lock);
		return 0;
	}
	spin_unlock(&tiqn->tiqn_state_lock);

	return -1;
}

void iscsit_put_tiqn_for_login(struct iscsi_tiqn *tiqn)
{
	spin_lock(&tiqn->tiqn_state_lock);
	tiqn->tiqn_access_count--;
	spin_unlock(&tiqn->tiqn_state_lock);
}

/*
 * Note that IQN formatting is expected to be done in userspace, and
 * no explict IQN format checks are done here.
 */
struct iscsi_tiqn *iscsit_add_tiqn(unsigned char *buf)
{
	struct iscsi_tiqn *tiqn = NULL;
	int ret;

	if (strlen(buf) > ISCSI_IQN_LEN) {
		printk(KERN_ERR "Target IQN exceeds %d bytes\n",
				ISCSI_IQN_LEN);
		return ERR_PTR(-EINVAL);
	}

	tiqn = kzalloc(sizeof(struct iscsi_tiqn), GFP_KERNEL);
	if (!tiqn) {
		printk(KERN_ERR "Unable to allocate struct iscsi_tiqn\n");
		return ERR_PTR(-ENOMEM);
	}

	sprintf(tiqn->tiqn, "%s", buf);
	INIT_LIST_HEAD(&tiqn->tiqn_list);
	INIT_LIST_HEAD(&tiqn->tiqn_tpg_list);
	spin_lock_init(&tiqn->tiqn_state_lock);
	spin_lock_init(&tiqn->tiqn_tpg_lock);
	spin_lock_init(&tiqn->sess_err_stats.lock);
	spin_lock_init(&tiqn->login_stats.lock);
	spin_lock_init(&tiqn->logout_stats.lock);

	if (!idr_pre_get(&tiqn_idr, GFP_KERNEL)) {
		printk(KERN_ERR "idr_pre_get() for tiqn_idr failed\n");
		kfree(tiqn);
		return ERR_PTR(-ENOMEM);
	}
	tiqn->tiqn_state = TIQN_STATE_ACTIVE;

	spin_lock(&tiqn_lock);
	ret = idr_get_new(&tiqn_idr, NULL, &tiqn->tiqn_index);
	if (ret < 0) {
		printk("idr_get_new() failed for tiqn->tiqn_index\n");
		spin_unlock(&tiqn_lock);
		kfree(tiqn);
		return ERR_PTR(ret);
	}
	list_add_tail(&tiqn->tiqn_list, &g_tiqn_list);
	spin_unlock(&tiqn_lock);

	printk(KERN_INFO "CORE[0] - Added iSCSI Target IQN: %s\n", tiqn->tiqn);

	return tiqn;

}

static void iscsit_wait_for_tiqn(struct iscsi_tiqn *tiqn)
{
	/*
	 * Wait for accesses to said struct iscsi_tiqn to end.
	 */
	spin_lock(&tiqn->tiqn_state_lock);
	while (tiqn->tiqn_access_count != 0) {
		spin_unlock(&tiqn->tiqn_state_lock);
		msleep(10);
		spin_lock(&tiqn->tiqn_state_lock);
	}
	spin_unlock(&tiqn->tiqn_state_lock);
}

void iscsit_del_tiqn(struct iscsi_tiqn *tiqn)
{
	/*
	 * iscsit_set_tiqn_shutdown sets tiqn->tiqn_state = TIQN_STATE_SHUTDOWN
	 * while holding tiqn->tiqn_state_lock.  This means that all subsequent
	 * attempts to access this struct iscsi_tiqn will fail from both transport
	 * fabric and control code paths.
	 */
	if (iscsit_set_tiqn_shutdown(tiqn) < 0) {
		printk(KERN_ERR "iscsit_set_tiqn_shutdown() failed\n");
		return;
	}

	iscsit_wait_for_tiqn(tiqn);

	spin_lock(&tiqn_lock);
	list_del(&tiqn->tiqn_list);
	idr_remove(&tiqn_idr, tiqn->tiqn_index);
	spin_unlock(&tiqn_lock);

	printk(KERN_INFO "CORE[0] - Deleted iSCSI Target IQN: %s\n",
			tiqn->tiqn);
	kfree(tiqn);
}

int iscsit_access_np(struct iscsi_np *np, struct iscsi_portal_group *tpg)
{
	int ret;
	/*
	 * Determine if the network portal is accepting storage traffic.
	 */
	spin_lock_bh(&np->np_thread_lock);
	if (np->np_thread_state != ISCSI_NP_THREAD_ACTIVE) {
		spin_unlock_bh(&np->np_thread_lock);
		return -1;
	}
	if (np->np_login_tpg) {
		printk(KERN_ERR "np->np_login_tpg() is not NULL!\n");
		spin_unlock_bh(&np->np_thread_lock);
		return -1;
	}
	spin_unlock_bh(&np->np_thread_lock);
	/*
	 * Determine if the portal group is accepting storage traffic.
	 */
	spin_lock_bh(&tpg->tpg_state_lock);
	if (tpg->tpg_state != TPG_STATE_ACTIVE) {
		spin_unlock_bh(&tpg->tpg_state_lock);
		return -1;
	}
	spin_unlock_bh(&tpg->tpg_state_lock);

	/*
	 * Here we serialize access across the TIQN+TPG Tuple.
	 */
	ret = mutex_lock_interruptible(&tpg->np_login_lock);
	if ((ret != 0) || signal_pending(current))
		return -1;

	spin_lock_bh(&np->np_thread_lock);
	np->np_login_tpg = tpg;
	spin_unlock_bh(&np->np_thread_lock);

	return 0;
}

int iscsit_deaccess_np(struct iscsi_np *np, struct iscsi_portal_group *tpg)
{
	struct iscsi_tiqn *tiqn = tpg->tpg_tiqn;

	spin_lock_bh(&np->np_thread_lock);
	np->np_login_tpg = NULL;
	spin_unlock_bh(&np->np_thread_lock);

	mutex_unlock(&tpg->np_login_lock);

	if (tiqn)
		iscsit_put_tiqn_for_login(tiqn);

	return 0;
}

static struct iscsi_np *iscsit_get_np(
	struct __kernel_sockaddr_storage *sockaddr,
	int network_transport)
{
	struct sockaddr_in *sock_in, *sock_in_e;
	struct sockaddr_in6 *sock_in6, *sock_in6_e;
	struct iscsi_np *np;
	int ip_match = 0;
	u16 port;

	spin_lock_bh(&np_lock);
	list_for_each_entry(np, &g_np_list, np_list) {
		spin_lock(&np->np_thread_lock);
		if (np->np_thread_state != ISCSI_NP_THREAD_ACTIVE) {
			spin_unlock(&np->np_thread_lock);
			continue;
		}

		if (sockaddr->ss_family == AF_INET6) {
			sock_in6 = (struct sockaddr_in6 *)sockaddr;
			sock_in6_e = (struct sockaddr_in6 *)&np->np_sockaddr;

			if (!memcmp((void *)&sock_in6->sin6_addr.in6_u,
				    (void *)&sock_in6_e->sin6_addr.in6_u,
				    sizeof(struct in6_addr)))
				ip_match = 1;

			port = ntohs(sock_in6->sin6_port);
		} else {
			sock_in = (struct sockaddr_in *)sockaddr;
			sock_in_e = (struct sockaddr_in *)&np->np_sockaddr;

			if (sock_in->sin_addr.s_addr ==
			    sock_in_e->sin_addr.s_addr)
				ip_match = 1;

			port = ntohs(sock_in->sin_port);
		}
			
		if ((ip_match == 1) && (np->np_port == port) &&
		    (np->np_network_transport == network_transport)) {
			/*
			 * Increment the np_exports reference count now to
			 * prevent iscsit_del_np() below from being called
			 * while iscsi_tpg_add_network_portal() is called.
			 */
			np->np_exports++;
			spin_unlock(&np->np_thread_lock);
			spin_unlock_bh(&np_lock);
			return np;
		}
		spin_unlock(&np->np_thread_lock);
	}
	spin_unlock_bh(&np_lock);

	return NULL;
}

struct iscsi_np *iscsit_add_np(
	struct __kernel_sockaddr_storage *sockaddr,
	char *ip_str,
	int network_transport)
{
	struct sockaddr_in *sock_in;
	struct sockaddr_in6 *sock_in6;
	struct iscsi_np *np;
	int ret;
	/*
	 * Locate the existing struct iscsi_np if already active..
	 */
	np = iscsit_get_np(sockaddr, network_transport);
	if (np)
		return np;

	np = kzalloc(sizeof(struct iscsi_np), GFP_KERNEL);
	if (!np) {
		printk(KERN_ERR "Unable to allocate memory for struct iscsi_np\n");
		return ERR_PTR(-ENOMEM);
	}

	np->np_flags |= NPF_IP_NETWORK;
	if (sockaddr->ss_family == AF_INET6) {
		sock_in6 = (struct sockaddr_in6 *)sockaddr;
		snprintf(np->np_ip, IPV6_ADDRESS_SPACE, "%s", ip_str);
		np->np_port = ntohs(sock_in6->sin6_port);
	} else {
		sock_in = (struct sockaddr_in *)sockaddr;
		sprintf(np->np_ip, "%s", ip_str);
		np->np_port = ntohs(sock_in->sin_port);
	}

	np->np_network_transport = network_transport;
	spin_lock_init(&np->np_thread_lock);
	init_completion(&np->np_restart_comp);
	INIT_LIST_HEAD(&np->np_list);

	ret = iscsi_target_setup_login_socket(np, sockaddr);
	if (ret != 0) {
		kfree(np);
		return ERR_PTR(ret);
	}

	np->np_thread = kthread_run(iscsi_target_login_thread, np, "iscsi_np");
	if (IS_ERR(np->np_thread)) {
		printk(KERN_ERR "Unable to create kthread: iscsi_np\n");
		ret = PTR_ERR(np->np_thread);
		kfree(np);
		return ERR_PTR(ret);
	}
	/*
	 * Increment the np_exports reference count now to prevent
	 * iscsit_del_np() below from being run while a new call to
	 * iscsi_tpg_add_network_portal() for a matching iscsi_np is
	 * active.  We don't need to hold np->np_thread_lock at this
	 * point because iscsi_np has not been added to g_np_list yet.
	 */
	np->np_exports = 1;

	spin_lock_bh(&np_lock);
	list_add_tail(&np->np_list, &g_np_list);
	spin_unlock_bh(&np_lock);

	printk(KERN_INFO "CORE[0] - Added Network Portal: %s:%hu on %s\n",
		np->np_ip, np->np_port, (np->np_network_transport == ISCSI_TCP) ?
		"TCP" : "SCTP");

	return np;
}

int iscsit_reset_np_thread(
	struct iscsi_np *np,
	struct iscsi_tpg_np *tpg_np,
	struct iscsi_portal_group *tpg)
{
	spin_lock_bh(&np->np_thread_lock);
	if (tpg && tpg_np) {
		/*
		 * The reset operation need only be performed when the
		 * passed struct iscsi_portal_group has a login in progress
		 * to one of the network portals.
		 */
		if (tpg_np->tpg_np->np_login_tpg != tpg) {
			spin_unlock_bh(&np->np_thread_lock);
			return 0;
		}
	}
	if (np->np_thread_state == ISCSI_NP_THREAD_INACTIVE) {
		spin_unlock_bh(&np->np_thread_lock);
		return 0;
	}
	np->np_thread_state = ISCSI_NP_THREAD_RESET;

	if (np->np_thread) {
		spin_unlock_bh(&np->np_thread_lock);
		send_sig(SIGINT, np->np_thread, 1);
		wait_for_completion(&np->np_restart_comp);
		spin_lock_bh(&np->np_thread_lock);
	}
	spin_unlock_bh(&np->np_thread_lock);

	return 0;
}

int iscsit_del_np_comm(struct iscsi_np *np)
{
	if (!np->np_socket)
		return 0;

	/*
	 * Some network transports allocate their own struct sock->file,
	 * see  if we need to free any additional allocated resources.
	 */
	if (np->np_flags & NPF_SCTP_STRUCT_FILE) {
		kfree(np->np_socket->file);
		np->np_socket->file = NULL;
	}

	sock_release(np->np_socket);
	return 0;
}

int iscsit_del_np(struct iscsi_np *np)
{
	spin_lock_bh(&np->np_thread_lock);
	if (!(--np->np_exports == 0)) {
		spin_unlock_bh(&np->np_thread_lock);
		return 0;
	}
	np->np_thread_state = ISCSI_NP_THREAD_SHUTDOWN;
	spin_unlock_bh(&np->np_thread_lock);

	if (np->np_thread) {
		/*
		 * We need to send the signal to wakeup Linux/Net
		 * which may be sleeping in sock_accept()..
		 */
		send_sig(SIGINT, np->np_thread, 1);
		kthread_stop(np->np_thread);
	}
	iscsit_del_np_comm(np);

	spin_lock_bh(&np_lock);
	list_del(&np->np_list);
	spin_unlock_bh(&np_lock);

	printk(KERN_INFO "CORE[0] - Removed Network Portal: %s:%hu on %s\n",
		np->np_ip, np->np_port, (np->np_network_transport == ISCSI_TCP) ?
		"TCP" : "SCTP");

	kfree(np);
	return 0;
}

static int __init iscsi_target_init_module(void)
{
	int ret = 0;

	printk(KERN_INFO "iSCSI-Target "ISCSIT_VERSION"\n");

	iscsit_global = kzalloc(sizeof(struct iscsit_global), GFP_KERNEL);
	if (!iscsit_global) {
		printk(KERN_ERR "Unable to allocate memory for iscsit_global\n");
		return -1;
	}
	mutex_init(&auth_id_lock);
	spin_lock_init(&sess_idr_lock);
	idr_init(&tiqn_idr);
	idr_init(&sess_idr);

	ret = iscsi_target_register_configfs();
	if (ret < 0)
		goto out;

	ret = iscsi_thread_set_init();
	if (ret < 0)
		goto configfs_out;

	if (iscsi_allocate_thread_sets(TARGET_THREAD_SET_COUNT) !=
			TARGET_THREAD_SET_COUNT) {
		printk(KERN_ERR "iscsi_allocate_thread_sets() returned"
			" unexpected value!\n");
		goto ts_out1;
	}

	lio_cmd_cache = kmem_cache_create("lio_cmd_cache",
			sizeof(struct iscsi_cmd), __alignof__(struct iscsi_cmd),
			0, NULL);
	if (!lio_cmd_cache) {
		printk(KERN_ERR "Unable to kmem_cache_create() for"
				" lio_cmd_cache\n");
		goto ts_out2;
	}

	lio_qr_cache = kmem_cache_create("lio_qr_cache",
			sizeof(struct iscsi_queue_req),
			__alignof__(struct iscsi_queue_req), 0, NULL);
	if (!lio_qr_cache) {
		printk(KERN_ERR "nable to kmem_cache_create() for"
				" lio_qr_cache\n");
		goto cmd_out;
	}

	lio_dr_cache = kmem_cache_create("lio_dr_cache",
			sizeof(struct iscsi_datain_req),
			__alignof__(struct iscsi_datain_req), 0, NULL);
	if (!lio_dr_cache) {
		printk(KERN_ERR "Unable to kmem_cache_create() for"
				" lio_dr_cache\n");
		goto qr_out;
	}

	lio_ooo_cache = kmem_cache_create("lio_ooo_cache",
			sizeof(struct iscsi_ooo_cmdsn),
			__alignof__(struct iscsi_ooo_cmdsn), 0, NULL);
	if (!lio_ooo_cache) {
		printk(KERN_ERR "Unable to kmem_cache_create() for"
				" lio_ooo_cache\n");
		goto dr_out;
	}

	lio_r2t_cache = kmem_cache_create("lio_r2t_cache",
			sizeof(struct iscsi_r2t), __alignof__(struct iscsi_r2t),
			0, NULL);
	if (!lio_r2t_cache) {
		printk(KERN_ERR "Unable to kmem_cache_create() for"
				" lio_r2t_cache\n");
		goto ooo_out;
	}

	if (iscsit_load_discovery_tpg() < 0)
		goto r2t_out;

	return ret;
r2t_out:
	kmem_cache_destroy(lio_r2t_cache);
ooo_out:
	kmem_cache_destroy(lio_ooo_cache);
dr_out:
	kmem_cache_destroy(lio_dr_cache);
qr_out:
	kmem_cache_destroy(lio_qr_cache);
cmd_out:
	kmem_cache_destroy(lio_cmd_cache);
ts_out2:
	iscsi_deallocate_thread_sets();
ts_out1:
	iscsi_thread_set_free();
configfs_out:
	iscsi_target_deregister_configfs();
out:
	kfree(iscsit_global);
	return -ENOMEM;
}

static void __exit iscsi_target_cleanup_module(void)
{
	iscsi_deallocate_thread_sets();
	iscsi_thread_set_free();
	iscsit_release_discovery_tpg();
	kmem_cache_destroy(lio_cmd_cache);
	kmem_cache_destroy(lio_qr_cache);
	kmem_cache_destroy(lio_dr_cache);
	kmem_cache_destroy(lio_ooo_cache);
	kmem_cache_destroy(lio_r2t_cache);

	iscsi_target_deregister_configfs();

	kfree(iscsit_global);
}

int iscsit_add_reject(
	u8 reason,
	int fail_conn,
	unsigned char *buf,
	struct iscsi_conn *conn)
{
	struct iscsi_cmd *cmd;
	struct iscsi_reject *hdr;
	int ret;

	cmd = iscsit_allocate_cmd(conn, GFP_KERNEL);
	if (!cmd)
		return -1;

	cmd->iscsi_opcode = ISCSI_OP_REJECT;
	if (fail_conn)
		cmd->cmd_flags |= ICF_REJECT_FAIL_CONN;

	hdr	= (struct iscsi_reject *) cmd->pdu;
	hdr->reason = reason;

	cmd->buf_ptr = kzalloc(ISCSI_HDR_LEN, GFP_KERNEL);
	if (!cmd->buf_ptr) {
		printk(KERN_ERR "Unable to allocate memory for cmd->buf_ptr\n");
		iscsit_release_cmd(cmd);
		return -1;
	}
	memcpy(cmd->buf_ptr, buf, ISCSI_HDR_LEN);

	spin_lock_bh(&conn->cmd_lock);
	list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
	spin_unlock_bh(&conn->cmd_lock);

	cmd->i_state = ISTATE_SEND_REJECT;
	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);

	ret = wait_for_completion_interruptible(&cmd->reject_comp);
	if (ret != 0)
		return -1;

	return (!fail_conn) ? 0 : -1;
}

int iscsit_add_reject_from_cmd(
	u8 reason,
	int fail_conn,
	int add_to_conn,
	unsigned char *buf,
	struct iscsi_cmd *cmd)
{
	struct iscsi_conn *conn;
	struct iscsi_reject *hdr;
	int ret;

	if (!cmd->conn) {
		printk(KERN_ERR "cmd->conn is NULL for ITT: 0x%08x\n",
				cmd->init_task_tag);
		return -1;
	}
	conn = cmd->conn;

	cmd->iscsi_opcode = ISCSI_OP_REJECT;
	if (fail_conn)
		cmd->cmd_flags |= ICF_REJECT_FAIL_CONN;

	hdr	= (struct iscsi_reject *) cmd->pdu;
	hdr->reason = reason;

	cmd->buf_ptr = kzalloc(ISCSI_HDR_LEN, GFP_KERNEL);
	if (!cmd->buf_ptr) {
		printk(KERN_ERR "Unable to allocate memory for cmd->buf_ptr\n");
		iscsit_release_cmd(cmd);
		return -1;
	}
	memcpy(cmd->buf_ptr, buf, ISCSI_HDR_LEN);

	if (add_to_conn) {
		spin_lock_bh(&conn->cmd_lock);
		list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
		spin_unlock_bh(&conn->cmd_lock);
	}

	cmd->i_state = ISTATE_SEND_REJECT;
	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);

	ret = wait_for_completion_interruptible(&cmd->reject_comp);
	if (ret != 0)
		return -1;

	return (!fail_conn) ? 0 : -1;
}

static void iscsit_calculate_map_segment(
	u32 *data_length,
	struct se_offset_map *lm)
{
	u32 sg_offset = 0;
	struct se_mem *se_mem = lm->map_se_mem;
	/*
	 * Still working on pages in the current struct se_mem.
	 */
	if (!lm->map_reset) {
		lm->iovec_length = (lm->sg_length > PAGE_SIZE) ?
					PAGE_SIZE : lm->sg_length;
		if (*data_length < lm->iovec_length)
			lm->iovec_length = *data_length;

		lm->iovec_base = page_address(lm->sg_page) + sg_offset;
		return;
	}

	/*
	 * First run of an iscsi_linux_map_t.
	 *
	 * OR:
	 *
	 * Mapped all of the pages in the current scatterlist, move
	 * on to the next one.
	 */
	lm->map_reset = 0;
	sg_offset = se_mem->se_off;
	lm->sg_page = se_mem->se_page;
	lm->sg_length = se_mem->se_len;
	/*
	 * Get the base and length of the current page for use with the iovec.
	 */
recalc:
	lm->iovec_length = (lm->sg_length > (PAGE_SIZE - sg_offset)) ?
			   (PAGE_SIZE - sg_offset) : lm->sg_length;
	/*
	 * See if there is any iSCSI offset we need to deal with.
	 */
	if (!lm->current_offset) {
		lm->iovec_base = page_address(lm->sg_page) + sg_offset;

		if (*data_length < lm->iovec_length)
			lm->iovec_length = *data_length;

		return;
	}

	/*
	 * We know the iSCSI offset is in the next page of the current
	 * scatterlist.  Increase the lm->sg_page pointer and try again.
	 */
	if (lm->current_offset >= lm->iovec_length) {
		lm->current_offset -= lm->iovec_length;
		lm->sg_length -= lm->iovec_length;
		lm->sg_page++;
		sg_offset = 0;

		goto recalc;
	}

	/*
	 * The iSCSI offset is in the current page, increment the iovec
	 * base and reduce iovec length.
	 */
	lm->iovec_base = page_address(lm->sg_page);

	lm->iovec_base += sg_offset;
	lm->iovec_base += lm->current_offset;

	if ((lm->iovec_length - lm->current_offset) < *data_length)
		lm->iovec_length -= lm->current_offset;
	else
		lm->iovec_length = *data_length;

	if ((lm->sg_length - lm->current_offset) < *data_length)
		lm->sg_length -= lm->current_offset;
	else
		lm->sg_length = *data_length;

	lm->current_offset = 0;
}

static int iscsit_get_offset(
	struct se_offset_map *lmap,
	struct se_unmap_sg *usg)
{
	u32 current_length = 0, current_iscsi_offset = lmap->iscsi_offset;
	u32 total_offset = 0;
	struct se_cmd *cmd = usg->se_cmd;
	struct se_mem *se_mem;

	list_for_each_entry(se_mem, &cmd->t_mem_list, se_list)
		break;

	if (!se_mem) {
		printk(KERN_ERR "Unable to locate se_mem from"
				" cmd->t_mem_list\n");
		return -1;
	}

	/*
	 * Locate the current offset from the passed iSCSI Offset.
	 */
	while (lmap->iscsi_offset != current_length) {
		/*
		 * The iSCSI Offset is within the current struct se_mem.
		 *
		 * Or:
		 *
		 * The iSCSI Offset is outside of the current struct se_mem.
		 * Recalculate the values and obtain the next struct se_mem pointer.
		 */
		total_offset += se_mem->se_len;

		if (total_offset > lmap->iscsi_offset) {
			current_length += current_iscsi_offset;
			lmap->orig_offset = lmap->current_offset =
				usg->t_offset = current_iscsi_offset;
		} else {
			current_length += se_mem->se_len;
			current_iscsi_offset -= se_mem->se_len;

			list_for_each_entry_continue(se_mem,
					&cmd->t_mem_list, se_list)
				break;

			if (!se_mem) {
				printk(KERN_ERR "Unable to locate struct se_mem\n");
				return -1;
			}
		}
	}
	lmap->map_orig_se_mem = se_mem;
	usg->cur_se_mem = se_mem;

	return 0;
}

static int iscsit_set_iovec_ptrs(
	struct se_map_sg *map_sg,
	struct se_unmap_sg *unmap_sg)
{
	u32 i = 0 /* For iovecs */, j = 0 /* For scatterlists */;
	struct se_cmd *cmd = map_sg->se_cmd;
	struct iscsi_cmd *i_cmd = container_of(cmd, struct iscsi_cmd, se_cmd);
	struct se_offset_map *lmap = &unmap_sg->lmap;
	struct kvec *iov = map_sg->iov;

	/*
	 * Used for non scatterlist operations, assume a single iovec.
	 */
	if (!cmd->t_tasks_se_num) {
		iov[0].iov_base = (unsigned char *) cmd->t_task_buf +
							map_sg->data_offset;
		iov[0].iov_len  = map_sg->data_length;
		return 1;
	}

	/*
	 * Set lmap->map_reset = 1 so the first call to
	 * iscsit_calculate_map_segment() sets up the initial
	 * values for struct se_offset_map.
	 */
	lmap->map_reset = 1;
	/*
	 * Get a pointer to the first used scatterlist based on the passed
	 * offset. Also set the rest of the needed values in iscsi_linux_map_t.
	 */
	lmap->iscsi_offset = map_sg->data_offset;
	if (map_sg->sg_kmap_active) {
		unmap_sg->se_cmd = map_sg->se_cmd;
		iscsit_get_offset(lmap, unmap_sg);
		unmap_sg->data_length = map_sg->data_length;
	} else {
		lmap->current_offset = lmap->orig_offset;
	}
	lmap->map_se_mem = lmap->map_orig_se_mem;

	while (map_sg->data_length) {
		/*
		 * Time to get the virtual address for use with iovec pointers.
		 * This function will return the expected iovec_base address
		 * and iovec_length.
		 */
		iscsit_calculate_map_segment(&map_sg->data_length, lmap);

		/*
		 * Set the iov.iov_base and iov.iov_len from the current values
		 * in iscsi_linux_map_t.
		 */
		iov[i].iov_base = lmap->iovec_base;
		iov[i].iov_len = lmap->iovec_length;

		/*
		 * Subtract the final iovec length from the total length to be
		 * mapped, and the length of the current scatterlist.  Also
		 * perform the paranoid check to make sure we are not going to
		 * overflow the iovecs allocated for this command in the next
		 * pass.
		 */
		map_sg->data_length -= iov[i].iov_len;
		lmap->sg_length -= iov[i].iov_len;

		if ((++i + 1) > i_cmd->orig_iov_data_count) {
			printk(KERN_ERR "Current iovec count %u is greater than"
				" struct se_cmd->orig_data_iov_count %u, cannot"
				" continue.\n", i+1, i_cmd->orig_iov_data_count);
			return -1;
		}

		/*
		 * All done mapping this scatterlist's pages, move on to
		 * the next scatterlist by setting lmap.map_reset = 1;
		 */
		if (!lmap->sg_length || !map_sg->data_length) {
			list_for_each_entry(lmap->map_se_mem,
					&lmap->map_se_mem->se_list, se_list)
				break;

			if (!lmap->map_se_mem) {
				printk(KERN_ERR "Unable to locate next"
					" lmap->map_struct se_mem entry\n");
				return -1;
			}
			j++;

			lmap->sg_page = NULL;
			lmap->map_reset = 1;
		} else
			lmap->sg_page++;
	}

	unmap_sg->sg_count = j;

	return i;
}

static void iscsit_map_SG_segments(struct se_unmap_sg *unmap_sg)
{
	u32 i = 0;
	struct se_cmd *cmd = unmap_sg->se_cmd;
	struct se_mem *se_mem = unmap_sg->cur_se_mem;

	if (!cmd->t_tasks_se_num)
		return;

	list_for_each_entry_continue(se_mem, &cmd->t_mem_list, se_list) {
		kmap(se_mem->se_page);

		if (++i == unmap_sg->sg_count)
			break;
	}
}

static void iscsit_unmap_SG_segments(struct se_unmap_sg *unmap_sg)
{
	u32 i = 0;
	struct se_cmd *cmd = unmap_sg->se_cmd;
	struct se_mem *se_mem = unmap_sg->cur_se_mem;

	if (!cmd->t_tasks_se_num)
		return;

	list_for_each_entry_continue(se_mem, &cmd->t_mem_list, se_list) {
		kunmap(se_mem->se_page);

		if (++i == unmap_sg->sg_count)
			break;
	}
}

static void iscsit_ack_from_expstatsn(struct iscsi_conn *conn, u32 exp_statsn)
{
	struct iscsi_cmd *cmd;

	conn->exp_statsn = exp_statsn;

	spin_lock_bh(&conn->cmd_lock);
	list_for_each_entry(cmd, &conn->conn_cmd_list, i_list) {
		spin_lock(&cmd->istate_lock);
		if ((cmd->i_state == ISTATE_SENT_STATUS) &&
		    (cmd->stat_sn < exp_statsn)) {
			cmd->i_state = ISTATE_REMOVE;
			spin_unlock(&cmd->istate_lock);
			iscsit_add_cmd_to_immediate_queue(cmd, conn,
						cmd->i_state);
			continue;
		}
		spin_unlock(&cmd->istate_lock);
	}
	spin_unlock_bh(&conn->cmd_lock);
}

static int iscsit_handle_scsi_cmd(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	int	data_direction, cmdsn_ret = 0, immed_ret, ret, transport_ret;
	int	dump_immediate_data = 0, send_check_condition = 0, payload_length;
	struct iscsi_cmd	*cmd = NULL;
	struct iscsi_scsi_req *hdr;

	spin_lock_bh(&conn->sess->session_stats_lock);
	conn->sess->cmd_pdus++;
	if (conn->sess->se_sess->se_node_acl) {
		spin_lock(&conn->sess->se_sess->se_node_acl->stats_lock);
		conn->sess->se_sess->se_node_acl->num_cmds++;
		spin_unlock(&conn->sess->se_sess->se_node_acl->stats_lock);
	}
	spin_unlock_bh(&conn->sess->session_stats_lock);

	hdr			= (struct iscsi_scsi_req *) buf;
	payload_length		= ntoh24(hdr->dlength);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->data_length	= be32_to_cpu(hdr->data_length);
	hdr->cmdsn		= be32_to_cpu(hdr->cmdsn);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);

	/* FIXME; Add checks for AdditionalHeaderSegment */

	if (!(hdr->flags & ISCSI_FLAG_CMD_WRITE) &&
	    !(hdr->flags & ISCSI_FLAG_CMD_FINAL)) {
		printk(KERN_ERR "ISCSI_FLAG_CMD_WRITE & ISCSI_FLAG_CMD_FINAL"
				" not set. Bad iSCSI Initiator.\n");
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
				buf, conn);
	}

	if (((hdr->flags & ISCSI_FLAG_CMD_READ) ||
	     (hdr->flags & ISCSI_FLAG_CMD_WRITE)) && !hdr->data_length) {
		/*
		 * Vmware ESX v3.0 uses a modified Cisco Initiator (v3.4.2)
		 * that adds support for RESERVE/RELEASE.  There is a bug
		 * add with this new functionality that sets R/W bits when
		 * neither CDB carries any READ or WRITE datapayloads.
		 */
		if ((hdr->cdb[0] == 0x16) || (hdr->cdb[0] == 0x17)) {
			hdr->flags &= ~ISCSI_FLAG_CMD_READ;
			hdr->flags &= ~ISCSI_FLAG_CMD_WRITE;
			goto done;
		}

		printk(KERN_ERR "ISCSI_FLAG_CMD_READ or ISCSI_FLAG_CMD_WRITE"
			" set when Expected Data Transfer Length is 0 for"
			" CDB: 0x%02x. Bad iSCSI Initiator.\n", hdr->cdb[0]);
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
				buf, conn);
	}
done:

	if (!(hdr->flags & ISCSI_FLAG_CMD_READ) &&
	    !(hdr->flags & ISCSI_FLAG_CMD_WRITE) && (hdr->data_length != 0)) {
		printk(KERN_ERR "ISCSI_FLAG_CMD_READ and/or ISCSI_FLAG_CMD_WRITE"
			" MUST be set if Expected Data Transfer Length is not 0."
			" Bad iSCSI Initiator\n");
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
				buf, conn);
	}

	if ((hdr->flags & ISCSI_FLAG_CMD_READ) &&
	    (hdr->flags & ISCSI_FLAG_CMD_WRITE)) {
		printk(KERN_ERR "Bidirectional operations not supported!\n");
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
				buf, conn);
	}

	if (hdr->opcode & ISCSI_OP_IMMEDIATE) {
		printk(KERN_ERR "Illegally set Immediate Bit in iSCSI Initiator"
				" Scsi Command PDU.\n");
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
				buf, conn);
	}

	if (payload_length && !conn->sess->sess_ops->ImmediateData) {
		printk(KERN_ERR "ImmediateData=No but DataSegmentLength=%u,"
			" protocol error.\n", payload_length);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
				buf, conn);
	}

	if ((hdr->data_length == payload_length) &&
	    (!(hdr->flags & ISCSI_FLAG_CMD_FINAL))) {
		printk(KERN_ERR "Expected Data Transfer Length and Length of"
			" Immediate Data are the same, but ISCSI_FLAG_CMD_FINAL"
			" bit is not set protocol error\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
				buf, conn);
	}

	if (payload_length > hdr->data_length) {
		printk(KERN_ERR "DataSegmentLength: %u is greater than"
			" EDTL: %u, protocol error.\n", payload_length,
				hdr->data_length);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
				buf, conn);
	}

	if (payload_length > conn->conn_ops->MaxRecvDataSegmentLength) {
		printk(KERN_ERR "DataSegmentLength: %u is greater than"
			" MaxRecvDataSegmentLength: %u, protocol error.\n",
			payload_length, conn->conn_ops->MaxRecvDataSegmentLength);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
				buf, conn);
	}

	if (payload_length > conn->sess->sess_ops->FirstBurstLength) {
		printk(KERN_ERR "DataSegmentLength: %u is greater than"
			" FirstBurstLength: %u, protocol error.\n",
			payload_length, conn->sess->sess_ops->FirstBurstLength);
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_INVALID, 1,
					buf, conn);
	}

	data_direction = (hdr->flags & ISCSI_FLAG_CMD_WRITE) ? DMA_TO_DEVICE :
			 (hdr->flags & ISCSI_FLAG_CMD_READ) ? DMA_FROM_DEVICE :
			  DMA_NONE;

	cmd = iscsit_allocate_se_cmd(conn, hdr->data_length, data_direction,
				(hdr->flags & ISCSI_FLAG_CMD_ATTR_MASK));
	if (!cmd)
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_NO_RESOURCES, 1,
					buf, conn);

	TRACE(TRACE_ISCSI, "Got SCSI Command, ITT: 0x%08x, CmdSN: 0x%08x,"
		" ExpXferLen: %u, Length: %u, CID: %hu\n", hdr->itt,
		hdr->cmdsn, hdr->data_length, payload_length, conn->cid);

	cmd->iscsi_opcode	= ISCSI_OP_SCSI_CMD;
	cmd->i_state		= ISTATE_NEW_CMD;
	cmd->immediate_cmd	= ((hdr->opcode & ISCSI_OP_IMMEDIATE) ? 1 : 0);
	cmd->immediate_data	= (payload_length) ? 1 : 0;
	cmd->unsolicited_data	= ((!(hdr->flags & ISCSI_FLAG_CMD_FINAL) &&
				     (hdr->flags & ISCSI_FLAG_CMD_WRITE)) ? 1 : 0);
	if (cmd->unsolicited_data)
		cmd->cmd_flags |= ICF_NON_IMMEDIATE_UNSOLICITED_DATA;

	conn->sess->init_task_tag = cmd->init_task_tag = hdr->itt;
	if (hdr->flags & ISCSI_FLAG_CMD_READ) {
		spin_lock_bh(&conn->sess->ttt_lock);
		cmd->targ_xfer_tag = conn->sess->targ_xfer_tag++;
		if (cmd->targ_xfer_tag == 0xFFFFFFFF)
			cmd->targ_xfer_tag = conn->sess->targ_xfer_tag++;
		spin_unlock_bh(&conn->sess->ttt_lock);
	} else if (hdr->flags & ISCSI_FLAG_CMD_WRITE)
		cmd->targ_xfer_tag = 0xFFFFFFFF;
	cmd->cmd_sn		= hdr->cmdsn;
	cmd->exp_stat_sn	= hdr->exp_statsn;
	cmd->first_burst_len	= payload_length;

	if (cmd->data_direction == DMA_FROM_DEVICE) {
		struct iscsi_datain_req *dr;

		dr = iscsit_allocate_datain_req();
		if (!dr)
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_BOOKMARK_NO_RESOURCES,
					1, 1, buf, cmd);

		iscsit_attach_datain_req(cmd, dr);
	}

	/*
	 * The CDB is going to an se_device_t.
	 */
	ret = iscsit_get_lun_for_cmd(cmd, hdr->cdb,
				get_unaligned_le64(&hdr->lun[0]));
	if (ret < 0) {
		if (SE_CMD(cmd)->scsi_sense_reason == TCM_NON_EXISTENT_LUN) {
			TRACE(TRACE_VANITY, "Responding to non-acl'ed,"
				" non-existent or non-exported iSCSI LUN:"
				" 0x%016Lx\n", get_unaligned_le64(&hdr->lun[0]));
		}
		if (ret == PYX_TRANSPORT_OUT_OF_MEMORY_RESOURCES)
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_BOOKMARK_NO_RESOURCES,
					1, 1, buf, cmd);

		send_check_condition = 1;
		goto attach_cmd;
	}
	/*
	 * The Initiator Node has access to the LUN (the addressing method
	 * is handled inside of iscsit_get_lun_for_cmd()).  Now it's time to
	 * allocate 1->N transport tasks (depending on sector count and
	 * maximum request size the physical HBA(s) can handle.
	 */
	transport_ret = transport_generic_allocate_tasks(SE_CMD(cmd), hdr->cdb);
	if (transport_ret == -ENOMEM) {
		return iscsit_add_reject_from_cmd(
				ISCSI_REASON_BOOKMARK_NO_RESOURCES,
				1, 1, buf, cmd);
	} else if (transport_ret == -EINVAL) {
		/*
		 * Unsupported SAM Opcode.  CHECK_CONDITION will be sent
		 * in iscsit_execute_cmd() during the CmdSN OOO Execution
		 * Mechinism.
		 */
		send_check_condition = 1;
	} else {
		if (iscsit_decide_list_to_build(cmd, payload_length) < 0)
			return iscsit_add_reject_from_cmd(
				ISCSI_REASON_BOOKMARK_NO_RESOURCES,
				1, 1, buf, cmd);
	}

attach_cmd:
	spin_lock_bh(&conn->cmd_lock);
	list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
	spin_unlock_bh(&conn->cmd_lock);
	/*
	 * Check if we need to delay processing because of ALUA
	 * Active/NonOptimized primary access state..
	 */
	core_alua_check_nonop_delay(SE_CMD(cmd));
	/*
	 * Check the CmdSN against ExpCmdSN/MaxCmdSN here if
	 * the Immediate Bit is not set, and no Immediate
	 * Data is attached.
	 *
	 * A PDU/CmdSN carrying Immediate Data can only
	 * be processed after the DataCRC has passed.
	 * If the DataCRC fails, the CmdSN MUST NOT
	 * be acknowledged. (See below)
	 */
	if (!cmd->immediate_data) {
		cmdsn_ret = iscsit_check_received_cmdsn(conn,
				cmd, hdr->cmdsn);
		if ((cmdsn_ret == CMDSN_NORMAL_OPERATION) ||
		    (cmdsn_ret == CMDSN_HIGHER_THAN_EXP))
			do {} while (0);
		else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			cmd->i_state = ISTATE_REMOVE;
			iscsit_add_cmd_to_immediate_queue(cmd,
					conn, cmd->i_state);
			return 0;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
		}
	}
	iscsit_ack_from_expstatsn(conn, hdr->exp_statsn);

	/*
	 * If no Immediate Data is attached, it's OK to return now.
	 */
	if (!cmd->immediate_data) {
		if (send_check_condition)
			return 0;

		if (cmd->unsolicited_data) {
			iscsit_set_dataout_sequence_values(cmd);

			spin_lock_bh(&cmd->dataout_timeout_lock);
			iscsit_start_dataout_timer(cmd, cmd->conn);
			spin_unlock_bh(&cmd->dataout_timeout_lock);
		}

		return 0;
	}

	/*
	 * Early CHECK_CONDITIONs never make it to the transport processing
	 * thread.  They are processed in CmdSN order by
	 * iscsit_check_received_cmdsn() below.
	 */
	if (send_check_condition) {
		immed_ret = IMMEDIATE_DATA_NORMAL_OPERATION;
		dump_immediate_data = 1;
		goto after_immediate_data;
	}

	/*
	 * Immediate Data is present, send to the transport and block until
	 * the underlying transport plugin has allocated the buffer to
	 * receive the Immediate Write Data into.
	 */
	transport_generic_handle_cdb(SE_CMD(cmd));

	wait_for_completion(&cmd->unsolicited_data_comp);

	if (SE_CMD(cmd)->se_cmd_flags & SCF_SE_CMD_FAILED) {
		immed_ret = IMMEDIATE_DATA_NORMAL_OPERATION;
		dump_immediate_data = 1;
		goto after_immediate_data;
	}

	immed_ret = iscsit_handle_immediate_data(cmd, buf, payload_length);
after_immediate_data:
	if (immed_ret == IMMEDIATE_DATA_NORMAL_OPERATION) {
		/*
		 * A PDU/CmdSN carrying Immediate Data passed
		 * DataCRC, check against ExpCmdSN/MaxCmdSN if
		 * Immediate Bit is not set.
		 */
		cmdsn_ret = iscsit_check_received_cmdsn(conn,
				cmd, hdr->cmdsn);
		/*
		 * Special case for Unsupported SAM WRITE Opcodes
		 * and ImmediateData=Yes.
		 */
		if (dump_immediate_data) {
			if (iscsit_dump_data_payload(conn, payload_length, 1) < 0)
				return -1;
		} else if (cmd->unsolicited_data) {
			iscsit_set_dataout_sequence_values(cmd);

			spin_lock_bh(&cmd->dataout_timeout_lock);
			iscsit_start_dataout_timer(cmd, cmd->conn);
			spin_unlock_bh(&cmd->dataout_timeout_lock);
		}

		if (cmdsn_ret == CMDSN_NORMAL_OPERATION)
			return 0;
		else if (cmdsn_ret == CMDSN_HIGHER_THAN_EXP)
			return 0;
		else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			cmd->i_state = ISTATE_REMOVE;
			iscsit_add_cmd_to_immediate_queue(cmd,
					conn, cmd->i_state);
			return 0;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
		}
	} else if (immed_ret == IMMEDIATE_DATA_ERL1_CRC_FAILURE) {
		/*
		 * Immediate Data failed DataCRC and ERL>=1,
		 * silently drop this PDU and let the initiator
		 * plug the CmdSN gap.
		 *
		 * FIXME: Send Unsolicited NOPIN with reserved
		 * TTT here to help the initiator figure out
		 * the missing CmdSN, although they should be
		 * intelligent enough to determine the missing
		 * CmdSN and issue a retry to plug the sequence.
		 */
		cmd->i_state = ISTATE_REMOVE;
		iscsit_add_cmd_to_immediate_queue(cmd, conn, cmd->i_state);
	} else /* immed_ret == IMMEDIATE_DATA_CANNOT_RECOVER */
		return -1;

	return 0;
}

static void iscsit_do_crypto_hash_iovec(
	struct hash_desc *hash,
	struct kvec *iov,
	u32 counter,
	u32 padding,
	u8 *pad_bytes,
	u8 *data_crc)
{
	struct scatterlist sg;
	struct kvec *iov_ptr = iov;

	crypto_hash_init(hash);

	while (counter > 0) {
		sg_init_one(&sg, iov_ptr->iov_base,
				iov_ptr->iov_len);
		crypto_hash_update(hash, &sg, iov_ptr->iov_len);
		
		counter -= iov_ptr->iov_len;
		iov_ptr++;
	}

	if (padding) {
		sg_init_one(&sg, pad_bytes, padding);
		crypto_hash_update(hash, &sg, padding);
	}
	crypto_hash_final(hash, data_crc);
}

static void iscsit_do_crypto_hash_buf(
	struct hash_desc *hash,
	unsigned char *buf,
	u32 payload_length,
	u32 padding,
	u8 *pad_bytes,
	u8 *data_crc)
{
	struct scatterlist sg;

	crypto_hash_init(hash);

	sg_init_one(&sg, (u8 *)buf, payload_length);
	crypto_hash_update(hash, &sg, payload_length);

	if (padding) {
		sg_init_one(&sg, pad_bytes, padding);
		crypto_hash_update(hash, &sg, padding);
	}
	crypto_hash_final(hash, data_crc);
}

static int iscsit_handle_data_out(struct iscsi_conn *conn, unsigned char *buf)
{
	int iov_ret, ooo_cmdsn = 0, ret;
	u8 data_crc_failed = 0, *pad_bytes[4];
	u32 checksum, iov_count = 0, padding = 0, rx_got = 0;
	u32 rx_size = 0, payload_length;
	struct iscsi_cmd *cmd = NULL;
	struct se_cmd *se_cmd;
	struct se_map_sg map_sg;
	struct se_unmap_sg unmap_sg;
	struct iscsi_data *hdr;
	struct kvec *iov;
	unsigned long flags;

	hdr			= (struct iscsi_data *) buf;
	payload_length		= ntoh24(hdr->dlength);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->ttt		= be32_to_cpu(hdr->ttt);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);
	hdr->datasn		= be32_to_cpu(hdr->datasn);
	hdr->offset		= be32_to_cpu(hdr->offset);

	if (!payload_length) {
		printk(KERN_ERR "DataOUT payload is ZERO, protocol error.\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	/* iSCSI write */
	spin_lock_bh(&conn->sess->session_stats_lock);
	conn->sess->rx_data_octets += payload_length;
	if (conn->sess->se_sess->se_node_acl) {
		spin_lock(&conn->sess->se_sess->se_node_acl->stats_lock);
		conn->sess->se_sess->se_node_acl->write_bytes += payload_length;
		spin_unlock(&conn->sess->se_sess->se_node_acl->stats_lock);
	}
	spin_unlock_bh(&conn->sess->session_stats_lock);

	if (payload_length > conn->conn_ops->MaxRecvDataSegmentLength) {
		printk(KERN_ERR "DataSegmentLength: %u is greater than"
			" MaxRecvDataSegmentLength: %u\n", payload_length,
			conn->conn_ops->MaxRecvDataSegmentLength);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	cmd = iscsit_find_cmd_from_itt_or_dump(conn, hdr->itt,
			payload_length);
	if (!cmd)
		return 0;

	TRACE(TRACE_ISCSI, "Got DataOut ITT: 0x%08x, TTT: 0x%08x,"
		" DataSN: 0x%08x, Offset: %u, Length: %u, CID: %hu\n",
		hdr->itt, hdr->ttt, hdr->datasn, hdr->offset,
		payload_length, conn->cid);

	if (cmd->cmd_flags & ICF_GOT_LAST_DATAOUT) {
		printk(KERN_ERR "Command ITT: 0x%08x received DataOUT after"
			" last DataOUT received, dumping payload\n",
			cmd->init_task_tag);
		return iscsit_dump_data_payload(conn, payload_length, 1);
	}

	if (cmd->data_direction != DMA_TO_DEVICE) {
		printk(KERN_ERR "Command ITT: 0x%08x received DataOUT for a"
			" NON-WRITE command.\n", cmd->init_task_tag);
		return iscsit_add_reject_from_cmd(ISCSI_REASON_PROTOCOL_ERROR,
				1, 0, buf, cmd);
	}
	se_cmd = SE_CMD(cmd);
	iscsit_mod_dataout_timer(cmd);

	if ((hdr->offset + payload_length) > cmd->data_length) {
		printk(KERN_ERR "DataOut Offset: %u, Length %u greater than"
			" iSCSI Command EDTL %u, protocol error.\n",
			hdr->offset, payload_length, cmd->data_length);
		return iscsit_add_reject_from_cmd(ISCSI_REASON_BOOKMARK_INVALID,
				1, 0, buf, cmd);
	}

	if (cmd->unsolicited_data) {
		int dump_unsolicited_data = 0, wait_for_transport = 0;

		if (conn->sess->sess_ops->InitialR2T) {
			printk(KERN_ERR "Received unexpected unsolicited data"
				" while InitialR2T=Yes, protocol error.\n");
			transport_send_check_condition_and_sense(SE_CMD(cmd),
					TCM_UNEXPECTED_UNSOLICITED_DATA, 0);
			return -1;
		}
		/*
		 * Special case for dealing with Unsolicited DataOUT
		 * and Unsupported SAM WRITE Opcodes and SE resource allocation
		 * failures;
		 */
		spin_lock_irqsave(&se_cmd->t_state_lock, flags);
		/*
		 * Handle cases where we do or do not want to sleep on
		 * unsolicited_data_comp
		 *
		 * First, if TRANSPORT_WRITE_PENDING state has not been reached,
		 * we need assume we need to wait and sleep..
		 */
		 wait_for_transport =
				(se_cmd->t_state != TRANSPORT_WRITE_PENDING);
		/*
		 * For the ImmediateData=Yes cases, there will already be
		 * generic target memory allocated with the original
		 * ISCSI_OP_SCSI_CMD PDU, so do not sleep for that case.
		 *
		 * The last is a check for a delayed TASK_ABORTED status that
		 * means the data payload will be dropped because
		 * SCF_SE_CMD_FAILED has been set to indicate that an exception
		 * condition for this struct sse_cmd has occured in generic target
		 * code that requires us to drop payload.
		 */
		wait_for_transport =
				(se_cmd->t_state != TRANSPORT_WRITE_PENDING);
		if ((cmd->immediate_data != 0) ||
		    (atomic_read(&se_cmd->t_transport_aborted) != 0))
			wait_for_transport = 0;
		spin_unlock_irqrestore(&se_cmd->t_state_lock, flags);

		if (wait_for_transport)
			wait_for_completion(&cmd->unsolicited_data_comp);

		spin_lock_irqsave(&se_cmd->t_state_lock, flags);
		if (!(se_cmd->se_cmd_flags & SCF_SUPPORTED_SAM_OPCODE) ||
		     (se_cmd->se_cmd_flags & SCF_SE_CMD_FAILED))
			dump_unsolicited_data = 1;
		spin_unlock_irqrestore(&se_cmd->t_state_lock, flags);

		if (dump_unsolicited_data) {
			/*
			 * Check if a delayed TASK_ABORTED status needs to
			 * be sent now if the ISCSI_FLAG_CMD_FINAL has been
			 * received with the unsolicitied data out.
			 */
			if (hdr->flags & ISCSI_FLAG_CMD_FINAL)
				iscsit_stop_dataout_timer(cmd);

			transport_check_aborted_status(se_cmd,
					(hdr->flags & ISCSI_FLAG_CMD_FINAL));
			return iscsit_dump_data_payload(conn, payload_length, 1);
		}
	} else {
		/*
		 * For the normal solicited data path:
		 *
		 * Check for a delayed TASK_ABORTED status and dump any
		 * incoming data out payload if one exists.  Also, when the
		 * ISCSI_FLAG_CMD_FINAL is set to denote the end of the current
		 * data out sequence, we decrement outstanding_r2ts.  Once
		 * outstanding_r2ts reaches zero, go ahead and send the delayed
		 * TASK_ABORTED status.
		 */
		if (atomic_read(&se_cmd->t_transport_aborted) != 0) {
			if (hdr->flags & ISCSI_FLAG_CMD_FINAL)
				if (--cmd->outstanding_r2ts < 1) {
					iscsit_stop_dataout_timer(cmd);
					transport_check_aborted_status(
							se_cmd, 1);
				}

			return iscsit_dump_data_payload(conn, payload_length, 1);
		}
	}
	/*
	 * Preform DataSN, DataSequenceInOrder, DataPDUInOrder, and
	 * within-command recovery checks before receiving the payload.
	 */
	ret = iscsit_check_pre_dataout(cmd, buf);
	if (ret == DATAOUT_WITHIN_COMMAND_RECOVERY)
		return 0;
	else if (ret == DATAOUT_CANNOT_RECOVER)
		return -1;

	rx_size += payload_length;
	iov = &cmd->iov_data[0];

	memset(&map_sg, 0, sizeof(struct se_map_sg));
	memset(&unmap_sg, 0, sizeof(struct se_unmap_sg));
	map_sg.fabric_cmd = (void *)cmd;
	map_sg.se_cmd = SE_CMD(cmd);
	map_sg.iov = iov;
	map_sg.sg_kmap_active = 1;
	map_sg.data_length = payload_length;
	map_sg.data_offset = hdr->offset;
	unmap_sg.fabric_cmd = (void *)cmd;
	unmap_sg.se_cmd = SE_CMD(cmd);

	iov_ret = iscsit_set_iovec_ptrs(&map_sg, &unmap_sg);
	if (iov_ret < 0)
		return -1;

	iov_count += iov_ret;

	padding = ((-payload_length) & 3);
	if (padding != 0) {
		iov[iov_count].iov_base	= &pad_bytes;
		iov[iov_count++].iov_len = padding;
		rx_size += padding;
		TRACE(TRACE_ISCSI, "Receiving %u padding bytes.\n", padding);
	}

	if (conn->conn_ops->DataDigest) {
		iov[iov_count].iov_base = &checksum;
		iov[iov_count++].iov_len = ISCSI_CRC_LEN;
		rx_size += ISCSI_CRC_LEN;
	}

	iscsit_map_SG_segments(&unmap_sg);

	rx_got = rx_data(conn, &cmd->iov_data[0], iov_count, rx_size);

	iscsit_unmap_SG_segments(&unmap_sg);

	if (rx_got != rx_size)
		return -1;

	if (conn->conn_ops->DataDigest) {
		u32 counter = payload_length, data_crc = 0;
		struct kvec *iov_ptr = &cmd->iov_data[0];
		/*
		 * Thanks to the IP stack shitting on passed iovecs,  we have to
		 * call set_iovec_data_ptrs() again in order to have a iMD/PSCSI
		 * agnostic way of doing datadigests computations.
		 */
		memset(&map_sg, 0, sizeof(struct se_map_sg));
		map_sg.fabric_cmd = (void *)cmd;
		map_sg.se_cmd = SE_CMD(cmd);
		map_sg.iov = iov_ptr;
		map_sg.data_length = payload_length;
		map_sg.data_offset = hdr->offset;

		if (iscsit_set_iovec_ptrs(&map_sg, &unmap_sg) < 0)
			return -1;

		iscsit_do_crypto_hash_iovec(&conn->conn_rx_hash,
				iov_ptr, counter, padding,
				(u8 *)&pad_bytes, (u8 *)&data_crc);

		if (checksum != data_crc) {
			printk(KERN_ERR "ITT: 0x%08x, Offset: %u, Length: %u,"
				" DataSN: 0x%08x, CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n",
				hdr->itt, hdr->offset, payload_length,
				hdr->datasn, checksum, data_crc);
			data_crc_failed = 1;
		} else {
			TRACE(TRACE_DIGEST, "Got CRC32C DataDigest 0x%08x for"
				" %u bytes of Data Out\n", checksum,
				payload_length);
		}
	}
	/*
	 * Increment post receive data and CRC values or perform
	 * within-command recovery.
	 */
	ret = iscsit_check_post_dataout(cmd, buf, data_crc_failed);
	if ((ret == DATAOUT_NORMAL) || (ret == DATAOUT_WITHIN_COMMAND_RECOVERY))
		return 0;
	else if (ret == DATAOUT_SEND_R2T) {
		iscsit_set_dataout_sequence_values(cmd);
		iscsit_build_r2ts_for_cmd(cmd, conn, 0);
	} else if (ret == DATAOUT_SEND_TO_TRANSPORT) {
		/*
		 * Handle extra special case for out of order
		 * Unsolicited Data Out.
		 */
		spin_lock_bh(&cmd->istate_lock);
		ooo_cmdsn = (cmd->cmd_flags & ICF_OOO_CMDSN);
		cmd->cmd_flags |= ICF_GOT_LAST_DATAOUT;
		cmd->i_state = ISTATE_RECEIVED_LAST_DATAOUT;
		spin_unlock_bh(&cmd->istate_lock);

		iscsit_stop_dataout_timer(cmd);
		return (!ooo_cmdsn) ? transport_generic_handle_data(
					SE_CMD(cmd)) : 0;
	} else /* DATAOUT_CANNOT_RECOVER */
		return -1;

	return 0;
}

static int iscsit_handle_nop_out(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	unsigned char *ping_data = NULL;
	int cmdsn_ret, niov = 0, ret = 0, rx_got, rx_size;
	u32 checksum, data_crc, padding = 0, payload_length;
	u64 lun;
	struct iscsi_cmd *cmd = NULL;
	struct kvec *iov = NULL;
	struct iscsi_nopout *hdr;

	hdr			= (struct iscsi_nopout *) buf;
	payload_length		= ntoh24(hdr->dlength);
	lun			= get_unaligned_le64(&hdr->lun[0]);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->ttt		= be32_to_cpu(hdr->ttt);
	hdr->cmdsn		= be32_to_cpu(hdr->cmdsn);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);

	if ((hdr->itt == 0xFFFFFFFF) && !(hdr->opcode & ISCSI_OP_IMMEDIATE)) {
		printk(KERN_ERR "NOPOUT ITT is reserved, but Immediate Bit is"
			" not set, protocol error.\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	if (payload_length > conn->conn_ops->MaxRecvDataSegmentLength) {
		printk(KERN_ERR "NOPOUT Ping Data DataSegmentLength: %u is"
			" greater than MaxRecvDataSegmentLength: %u, protocol"
			" error.\n", payload_length,
			conn->conn_ops->MaxRecvDataSegmentLength);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	TRACE(TRACE_ISCSI, "Got NOPOUT Ping %s ITT: 0x%08x, TTT: 0x%09x,"
		" CmdSN: 0x%08x, ExpStatSN: 0x%08x, Length: %u\n",
		(hdr->itt == 0xFFFFFFFF) ? "Response" : "Request",
		hdr->itt, hdr->ttt, hdr->cmdsn, hdr->exp_statsn,
		payload_length);
	/*
	 * This is not a response to a Unsolicited NopIN, which means
	 * it can either be a NOPOUT ping request (with a valid ITT),
	 * or a NOPOUT not requesting a NOPIN (with a reserved ITT).
	 * Either way, make sure we allocate an struct iscsi_cmd, as both
	 * can contain ping data.
	 */
	if (hdr->ttt == 0xFFFFFFFF) {
		cmd = iscsit_allocate_cmd(conn, GFP_KERNEL);
		if (!cmd)
			return iscsit_add_reject(
					ISCSI_REASON_BOOKMARK_NO_RESOURCES,
					1, buf, conn);

		cmd->iscsi_opcode	= ISCSI_OP_NOOP_OUT;
		cmd->i_state		= ISTATE_SEND_NOPIN;
		cmd->immediate_cmd	= ((hdr->opcode & ISCSI_OP_IMMEDIATE) ?
						1 : 0);
		conn->sess->init_task_tag = cmd->init_task_tag = hdr->itt;
		cmd->targ_xfer_tag	= 0xFFFFFFFF;
		cmd->cmd_sn		= hdr->cmdsn;
		cmd->exp_stat_sn	= hdr->exp_statsn;
		cmd->data_direction	= DMA_NONE;
	}

	if (payload_length && (hdr->ttt == 0xFFFFFFFF)) {
		rx_size = payload_length;
		ping_data = kzalloc(payload_length + 1, GFP_KERNEL);
		if (!ping_data) {
			printk(KERN_ERR "Unable to allocate memory for"
				" NOPOUT ping data.\n");
			ret = -1;
			goto out;
		}

		iov = &cmd->iov_misc[0];
		iov[niov].iov_base	= ping_data;
		iov[niov++].iov_len	= payload_length;

		padding = ((-payload_length) & 3);
		if (padding != 0) {
			TRACE(TRACE_ISCSI, "Receiving %u additional bytes"
				" for padding.\n", padding);
			iov[niov].iov_base	= &cmd->pad_bytes;
			iov[niov++].iov_len	= padding;
			rx_size += padding;
		}
		if (conn->conn_ops->DataDigest) {
			iov[niov].iov_base	= &checksum;
			iov[niov++].iov_len	= ISCSI_CRC_LEN;
			rx_size += ISCSI_CRC_LEN;
		}

		rx_got = rx_data(conn, &cmd->iov_misc[0], niov, rx_size);
		if (rx_got != rx_size) {
			ret = -1;
			goto out;
		}

		if (conn->conn_ops->DataDigest) {
			iscsit_do_crypto_hash_buf(&conn->conn_rx_hash,
					ping_data, payload_length,
					padding, (u8 *)&cmd->pad_bytes,
					(u8 *)&data_crc);

			if (checksum != data_crc) {
				printk(KERN_ERR "Ping data CRC32C DataDigest"
				" 0x%08x does not match computed 0x%08x\n",
					checksum, data_crc);
				if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
					printk(KERN_ERR "Unable to recover from"
					" NOPOUT Ping DataCRC failure while in"
						" ERL=0.\n");
					ret = -1;
					goto out;
				} else {
					/*
					 * Silently drop this PDU and let the
					 * initiator plug the CmdSN gap.
					 */
					TRACE(TRACE_ERL1, "Dropping NOPOUT"
					" Command CmdSN: 0x%08x due to"
					" DataCRC error.\n", hdr->cmdsn);
					ret = 0;
					goto out;
				}
			} else {
				TRACE(TRACE_DIGEST, "Got CRC32C DataDigest"
				" 0x%08x for %u bytes of ping data.\n",
					checksum, payload_length);
			}
		}

		ping_data[payload_length] = '\0';
		/*
		 * Attach ping data to struct iscsi_cmd->buf_ptr.
		 */
		cmd->buf_ptr = (void *)ping_data;
		cmd->buf_ptr_size = payload_length;

		TRACE(TRACE_ISCSI, "Got %u bytes of NOPOUT ping"
			" data.\n", payload_length);
		TRACE(TRACE_ISCSI, "Ping Data: \"%s\"\n", ping_data);
	}

	if (hdr->itt != 0xFFFFFFFF) {
		if (!cmd) {
			printk(KERN_ERR "Checking CmdSN for NOPOUT,"
				" but cmd is NULL!\n");
			return -1;
		}
		/*
		 * Initiator is expecting a NopIN ping reply,
		 */
		spin_lock_bh(&conn->cmd_lock);
		list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
		spin_unlock_bh(&conn->cmd_lock);

		iscsit_ack_from_expstatsn(conn, hdr->exp_statsn);

		if (hdr->opcode & ISCSI_OP_IMMEDIATE) {
			iscsit_add_cmd_to_response_queue(cmd, conn,
					cmd->i_state);
			return 0;
		}

		cmdsn_ret = iscsit_check_received_cmdsn(conn, cmd, hdr->cmdsn);
		if ((cmdsn_ret == CMDSN_NORMAL_OPERATION) ||
		    (cmdsn_ret == CMDSN_HIGHER_THAN_EXP)) {
			return 0;
		} else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			cmd->i_state = ISTATE_REMOVE;
			iscsit_add_cmd_to_immediate_queue(cmd, conn,
					cmd->i_state);
			ret = 0;
			goto ping_out;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
			ret = -1;
			goto ping_out;
		}

		return 0;
	}

	if (hdr->ttt != 0xFFFFFFFF) {
		/*
		 * This was a response to a unsolicited NOPIN ping.
		 */
		cmd = iscsit_find_cmd_from_ttt(conn, hdr->ttt);
		if (!cmd)
			return -1;

		iscsit_stop_nopin_response_timer(conn);

		cmd->i_state = ISTATE_REMOVE;
		iscsit_add_cmd_to_immediate_queue(cmd, conn, cmd->i_state);
		iscsit_start_nopin_timer(conn);
	} else {
		/*
		 * Initiator is not expecting a NOPIN is response.
		 * Just ignore for now.
		 *
		 * iSCSI v19-91 10.18
		 * "A NOP-OUT may also be used to confirm a changed
		 *  ExpStatSN if another PDU will not be available
		 *  for a long time."
		 */
		ret = 0;
		goto out;
	}

	return 0;
out:
	if (cmd)
		iscsit_release_cmd(cmd);
ping_out:
	kfree(ping_data);
	return ret;
}

static int iscsit_handle_task_mgt_cmd(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	struct iscsi_cmd *cmd;
	struct se_tmr_req *se_tmr;
	struct iscsi_tmr_req *tmr_req;
	struct iscsi_tm *hdr;
	u32 payload_length;
	int cmdsn_ret, out_of_order_cmdsn = 0, ret;
	u8 function;

	hdr			= (struct iscsi_tm *) buf;
	payload_length		= ntoh24(hdr->dlength);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->rtt		= be32_to_cpu(hdr->rtt);
	hdr->cmdsn		= be32_to_cpu(hdr->cmdsn);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);
	hdr->refcmdsn		= be32_to_cpu(hdr->refcmdsn);
	hdr->exp_datasn		= be32_to_cpu(hdr->exp_datasn);
	hdr->flags &= ~ISCSI_FLAG_CMD_FINAL;
	function = hdr->flags;

	TRACE(TRACE_ISCSI, "Got Task Management Request ITT: 0x%08x, CmdSN:"
		" 0x%08x, Function: 0x%02x, RefTaskTag: 0x%08x, RefCmdSN:"
		" 0x%08x, CID: %hu\n", hdr->itt, hdr->cmdsn, function,
		hdr->rtt, hdr->refcmdsn, conn->cid);

	if ((function != ISCSI_TM_FUNC_ABORT_TASK) &&
	    ((function != ISCSI_TM_FUNC_TASK_REASSIGN) &&
	     (hdr->rtt != ISCSI_RESERVED_TAG))) {
		printk(KERN_ERR "RefTaskTag should be set to 0xFFFFFFFF.\n");
		hdr->rtt = ISCSI_RESERVED_TAG;
	}

	if ((function == ISCSI_TM_FUNC_TASK_REASSIGN) &&
			!(hdr->opcode & ISCSI_OP_IMMEDIATE)) {
		printk(KERN_ERR "Task Management Request TASK_REASSIGN not"
			" issued as immediate command, bad iSCSI Initiator"
				"implementation\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}
	if ((function != ISCSI_TM_FUNC_ABORT_TASK) &&
	    (hdr->refcmdsn != ISCSI_RESERVED_TAG))
		hdr->refcmdsn = ISCSI_RESERVED_TAG;

	cmd = iscsit_allocate_se_cmd_for_tmr(conn, function);
	if (!cmd)
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_NO_RESOURCES,
					1, buf, conn);

	cmd->iscsi_opcode	= ISCSI_OP_SCSI_TMFUNC;
	cmd->i_state		= ISTATE_SEND_TASKMGTRSP;
	cmd->immediate_cmd	= ((hdr->opcode & ISCSI_OP_IMMEDIATE) ? 1 : 0);
	cmd->init_task_tag	= hdr->itt;
	cmd->targ_xfer_tag	= 0xFFFFFFFF;
	cmd->cmd_sn		= hdr->cmdsn;
	cmd->exp_stat_sn	= hdr->exp_statsn;
	se_tmr			= SE_CMD(cmd)->se_tmr_req;
	tmr_req			= cmd->tmr_req;
	/*
	 * Locate the struct se_lun for all TMRs not related to ERL=2 TASK_REASSIGN
	 */
	if (function != ISCSI_TM_FUNC_TASK_REASSIGN) {
		ret = iscsit_get_lun_for_tmr(cmd,
				get_unaligned_le64(&hdr->lun[0]));
		if (ret < 0) {
			SE_CMD(cmd)->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
			se_tmr->response = ISCSI_TMF_RSP_NO_LUN;
			goto attach;
		}
	}

	switch (function) {
	case ISCSI_TM_FUNC_ABORT_TASK:
		se_tmr->response = iscsit_tmr_abort_task(cmd, buf);
		if (se_tmr->response != ISCSI_TMF_RSP_COMPLETE) {
			SE_CMD(cmd)->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
			goto attach;
		}
		break;
	case ISCSI_TM_FUNC_ABORT_TASK_SET:
	case ISCSI_TM_FUNC_CLEAR_ACA:
	case ISCSI_TM_FUNC_CLEAR_TASK_SET:
	case ISCSI_TM_FUNC_LOGICAL_UNIT_RESET:
		break;
	case ISCSI_TM_FUNC_TARGET_WARM_RESET:
		if (iscsit_tmr_task_warm_reset(conn, tmr_req, buf) < 0) {
			SE_CMD(cmd)->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
			se_tmr->response = ISCSI_TMF_RSP_AUTH_FAILED;
			goto attach;
		}
		break;
	case ISCSI_TM_FUNC_TARGET_COLD_RESET:
		if (iscsit_tmr_task_cold_reset(conn, tmr_req, buf) < 0) {
			SE_CMD(cmd)->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
			se_tmr->response = ISCSI_TMF_RSP_AUTH_FAILED;
			goto attach;
		}
		break;
	case ISCSI_TM_FUNC_TASK_REASSIGN:
		se_tmr->response = iscsit_tmr_task_reassign(cmd, buf);
		/*
		 * Perform sanity checks on the ExpDataSN only if the
		 * TASK_REASSIGN was successful.
		 */
		if (se_tmr->response != ISCSI_TMF_RSP_COMPLETE)
			break;

		if (iscsit_check_task_reassign_expdatasn(tmr_req, conn) < 0)
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_BOOKMARK_INVALID, 1, 1,
					buf, cmd);
		break;
	default:
		printk(KERN_ERR "Unknown TMR function: 0x%02x, protocol"
			" error.\n", function);
		SE_CMD(cmd)->se_cmd_flags |= SCF_SCSI_CDB_EXCEPTION;
		se_tmr->response = ISCSI_TMF_RSP_NOT_SUPPORTED;
		goto attach;
	}

	if ((function != ISCSI_TM_FUNC_TASK_REASSIGN) &&
	    (se_tmr->response == ISCSI_TMF_RSP_COMPLETE))
		se_tmr->call_transport = 1;
attach:
	spin_lock_bh(&conn->cmd_lock);
	list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
	spin_unlock_bh(&conn->cmd_lock);

	if (!(hdr->opcode & ISCSI_OP_IMMEDIATE)) {
		cmdsn_ret = iscsit_check_received_cmdsn(conn,
				cmd, hdr->cmdsn);
		if (cmdsn_ret == CMDSN_NORMAL_OPERATION)
			do {} while (0);
		else if (cmdsn_ret == CMDSN_HIGHER_THAN_EXP)
			out_of_order_cmdsn = 1;
		else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			cmd->i_state = ISTATE_REMOVE;
			iscsit_add_cmd_to_immediate_queue(cmd, conn,
					cmd->i_state);
			return 0;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
		}
	}
	iscsit_ack_from_expstatsn(conn, hdr->exp_statsn);

	if (out_of_order_cmdsn)
		return 0;
	/*
	 * Found the referenced task, send to transport for processing.
	 */
	if (se_tmr->call_transport)
		return transport_generic_handle_tmr(SE_CMD(cmd));

	/*
	 * Could not find the referenced LUN, task, or Task Management
	 * command not authorized or supported.  Change state and
	 * let the tx_thread send the response.
	 *
	 * For connection recovery, this is also the default action for
	 * TMR TASK_REASSIGN.
	 */
	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);
	return 0;
}

/* #warning FIXME: Support Text Command parameters besides SendTargets */
static int iscsit_handle_text_cmd(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	char *text_ptr, *text_in;
	int cmdsn_ret, niov = 0, rx_got, rx_size;
	u32 checksum = 0, data_crc = 0, payload_length;
	u32 padding = 0, pad_bytes = 0, text_length = 0;
	struct iscsi_cmd *cmd;
	struct kvec iov[3];
	struct iscsi_text *hdr;

	hdr			= (struct iscsi_text *) buf;
	payload_length		= ntoh24(hdr->dlength);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->ttt		= be32_to_cpu(hdr->ttt);
	hdr->cmdsn		= be32_to_cpu(hdr->cmdsn);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);

	if (payload_length > conn->conn_ops->MaxRecvDataSegmentLength) {
		printk(KERN_ERR "Unable to accept text parameter length: %u"
			"greater than MaxRecvDataSegmentLength %u.\n",
		       payload_length, conn->conn_ops->MaxRecvDataSegmentLength);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	TRACE(TRACE_ISCSI, "Got Text Request: ITT: 0x%08x, CmdSN: 0x%08x,"
		" ExpStatSN: 0x%08x, Length: %u\n", hdr->itt, hdr->cmdsn,
		hdr->exp_statsn, payload_length);

	rx_size = text_length = payload_length;
	if (text_length) {
		text_in = kzalloc(text_length, GFP_KERNEL);
		if (!text_in) {
			printk(KERN_ERR "Unable to allocate memory for"
				" incoming text parameters\n");
			return -1;
		}

		memset(iov, 0, 3 * sizeof(struct kvec));
		iov[niov].iov_base	= text_in;
		iov[niov++].iov_len	= text_length;

		padding = ((-payload_length) & 3);
		if (padding != 0) {
			iov[niov].iov_base = &pad_bytes;
			iov[niov++].iov_len  = padding;
			rx_size += padding;
			TRACE(TRACE_ISCSI, "Receiving %u additional bytes"
					" for padding.\n", padding);
		}
		if (conn->conn_ops->DataDigest) {
			iov[niov].iov_base	= &checksum;
			iov[niov++].iov_len	= ISCSI_CRC_LEN;
			rx_size += ISCSI_CRC_LEN;
		}

		rx_got = rx_data(conn, &iov[0], niov, rx_size);
		if (rx_got != rx_size) {
			kfree(text_in);
			return -1;
		}

		if (conn->conn_ops->DataDigest) {
			iscsit_do_crypto_hash_buf(&conn->conn_rx_hash,
					text_in, text_length,
					padding, (u8 *)&pad_bytes,
					(u8 *)&data_crc);

			if (checksum != data_crc) {
				printk(KERN_ERR "Text data CRC32C DataDigest"
					" 0x%08x does not match computed"
					" 0x%08x\n", checksum, data_crc);
				if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
					printk(KERN_ERR "Unable to recover from"
					" Text Data digest failure while in"
						" ERL=0.\n");
					kfree(text_in);
					return -1;
				} else {
					/*
					 * Silently drop this PDU and let the
					 * initiator plug the CmdSN gap.
					 */
					TRACE(TRACE_ERL1, "Dropping Text"
					" Command CmdSN: 0x%08x due to"
					" DataCRC error.\n", hdr->cmdsn);
					kfree(text_in);
					return 0;
				}
			} else {
				TRACE(TRACE_DIGEST, "Got CRC32C DataDigest"
					" 0x%08x for %u bytes of text data.\n",
						checksum, text_length);
			}
		}
		text_in[text_length - 1] = '\0';
		TRACE(TRACE_ISCSI, "Successfully read %d bytes of text"
				" data.\n", text_length);

		if (strncmp("SendTargets", text_in, 11) != 0) {
			printk(KERN_ERR "Received Text Data that is not"
				" SendTargets, cannot continue.\n");
			kfree(text_in);
			return -1;
		}
		text_ptr = strchr(text_in, '=');
		if (!text_ptr) {
			printk(KERN_ERR "No \"=\" separator found in Text Data,"
				"  cannot continue.\n");
			kfree(text_in);
			return -1;
		}
		if (strncmp("=All", text_ptr, 4) != 0) {
			printk(KERN_ERR "Unable to locate All value for"
				" SendTargets key,  cannot continue.\n");
			kfree(text_in);
			return -1;
		}
/*#warning Support SendTargets=(iSCSI Target Name/Nothing) values. */
		kfree(text_in);
	}

	cmd = iscsit_allocate_cmd(conn, GFP_KERNEL);
	if (!cmd)
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_NO_RESOURCES,
					1, buf, conn);

	cmd->iscsi_opcode	= ISCSI_OP_TEXT;
	cmd->i_state		= ISTATE_SEND_TEXTRSP;
	cmd->immediate_cmd	= ((hdr->opcode & ISCSI_OP_IMMEDIATE) ? 1 : 0);
	conn->sess->init_task_tag = cmd->init_task_tag	= hdr->itt;
	cmd->targ_xfer_tag	= 0xFFFFFFFF;
	cmd->cmd_sn		= hdr->cmdsn;
	cmd->exp_stat_sn	= hdr->exp_statsn;
	cmd->data_direction	= DMA_NONE;

	spin_lock_bh(&conn->cmd_lock);
	list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
	spin_unlock_bh(&conn->cmd_lock);

	iscsit_ack_from_expstatsn(conn, hdr->exp_statsn);

	if (!(hdr->opcode & ISCSI_OP_IMMEDIATE)) {
		cmdsn_ret = iscsit_check_received_cmdsn(conn, cmd, hdr->cmdsn);
		if ((cmdsn_ret == CMDSN_NORMAL_OPERATION) ||
		     (cmdsn_ret == CMDSN_HIGHER_THAN_EXP))
			return 0;
		else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			iscsit_add_cmd_to_immediate_queue(cmd, conn,
						ISTATE_REMOVE);
			return 0;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
		}

		return 0;
	}

	return iscsit_execute_cmd(cmd, 0);
}

int iscsit_logout_closesession(struct iscsi_cmd *cmd, struct iscsi_conn *conn)
{
	struct iscsi_conn *conn_p;
	struct iscsi_session *sess = conn->sess;

	TRACE(TRACE_ISCSI, "Received logout request CLOSESESSION on CID: %hu"
		" for SID: %u.\n", conn->cid, conn->sess->sid);

	atomic_set(&sess->session_logout, 1);
	atomic_set(&conn->conn_logout_remove, 1);
	conn->conn_logout_reason = ISCSI_LOGOUT_REASON_CLOSE_SESSION;

	iscsit_inc_conn_usage_count(conn);
	iscsit_inc_session_usage_count(sess);

	spin_lock_bh(&sess->conn_lock);
	list_for_each_entry(conn_p, &sess->sess_conn_list, conn_list) {
		if (conn_p->conn_state != TARG_CONN_STATE_LOGGED_IN)
			continue;

		TRACE(TRACE_STATE, "Moving to TARG_CONN_STATE_IN_LOGOUT.\n");
		conn_p->conn_state = TARG_CONN_STATE_IN_LOGOUT;
	}
	spin_unlock_bh(&sess->conn_lock);

	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);

	return 0;
}

int iscsit_logout_closeconnection(struct iscsi_cmd *cmd, struct iscsi_conn *conn)
{
	struct iscsi_conn *l_conn;
	struct iscsi_session *sess = conn->sess;

	TRACE(TRACE_ISCSI, "Received logout request CLOSECONNECTION for CID:"
		" %hu on CID: %hu.\n", cmd->logout_cid, conn->cid);

	/*
	 * A Logout Request with a CLOSECONNECTION reason code for a CID
	 * can arrive on a connection with a differing CID.
	 */
	if (conn->cid == cmd->logout_cid) {
		spin_lock_bh(&conn->state_lock);
		TRACE(TRACE_STATE, "Moving to TARG_CONN_STATE_IN_LOGOUT.\n");
		conn->conn_state = TARG_CONN_STATE_IN_LOGOUT;

		atomic_set(&conn->conn_logout_remove, 1);
		conn->conn_logout_reason = ISCSI_LOGOUT_REASON_CLOSE_CONNECTION;
		iscsit_inc_conn_usage_count(conn);

		spin_unlock_bh(&conn->state_lock);
	} else {
		/*
		 * Handle all different cid CLOSECONNECTION requests in
		 * iscsit_logout_post_handler_diffcid() as to give enough
		 * time for any non immediate command's CmdSN to be
		 * acknowledged on the connection in question.
		 *
		 * Here we simply make sure the CID is still around.
		 */
		l_conn = iscsit_get_conn_from_cid(sess,
				cmd->logout_cid);
		if (!l_conn) {
			cmd->logout_response = ISCSI_LOGOUT_CID_NOT_FOUND;
			iscsit_add_cmd_to_response_queue(cmd, conn,
					cmd->i_state);
			return 0;
		}

		iscsit_dec_conn_usage_count(l_conn);
	}

	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);

	return 0;
}

int iscsit_logout_removeconnforrecovery(struct iscsi_cmd *cmd, struct iscsi_conn *conn)
{
	struct iscsi_session *sess = conn->sess;

	TRACE(TRACE_ERL2, "Received explicit REMOVECONNFORRECOVERY logout for"
		" CID: %hu on CID: %hu.\n", cmd->logout_cid, conn->cid);

	if (sess->sess_ops->ErrorRecoveryLevel != 2) {
		printk(KERN_ERR "Received Logout Request REMOVECONNFORRECOVERY"
			" while ERL!=2.\n");
		cmd->logout_response = ISCSI_LOGOUT_RECOVERY_UNSUPPORTED;
		iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);
		return 0;
	}

	if (conn->cid == cmd->logout_cid) {
		printk(KERN_ERR "Received Logout Request REMOVECONNFORRECOVERY"
			" with CID: %hu on CID: %hu, implementation error.\n",
				cmd->logout_cid, conn->cid);
		cmd->logout_response = ISCSI_LOGOUT_CLEANUP_FAILED;
		iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);
		return 0;
	}

	iscsit_add_cmd_to_response_queue(cmd, conn, cmd->i_state);

	return 0;
}

static int iscsit_handle_logout_cmd(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	int cmdsn_ret, logout_remove = 0;
	u8 reason_code = 0;
	struct iscsi_cmd *cmd;
	struct iscsi_logout *hdr;
	struct iscsi_tiqn *tiqn = iscsit_snmp_get_tiqn(conn);

	hdr			= (struct iscsi_logout *) buf;
	reason_code		= (hdr->flags & 0x7f);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->cid		= be16_to_cpu(hdr->cid);
	hdr->cmdsn		= be32_to_cpu(hdr->cmdsn);
	hdr->exp_statsn	= be32_to_cpu(hdr->exp_statsn);

	if (tiqn) {
		spin_lock(&tiqn->logout_stats.lock);
		if (reason_code == ISCSI_LOGOUT_REASON_CLOSE_SESSION)
			tiqn->logout_stats.normal_logouts++;
		else
			tiqn->logout_stats.abnormal_logouts++;
		spin_unlock(&tiqn->logout_stats.lock);
	}

	TRACE(TRACE_ISCSI, "Got Logout Request ITT: 0x%08x CmdSN: 0x%08x"
		" ExpStatSN: 0x%08x Reason: 0x%02x CID: %hu on CID: %hu\n",
		hdr->itt, hdr->cmdsn, hdr->exp_statsn, reason_code,
		hdr->cid, conn->cid);

	if (conn->conn_state != TARG_CONN_STATE_LOGGED_IN) {
		printk(KERN_ERR "Received logout request on connection that"
			" is not in logged in state, ignoring request.\n");
		return 0;
	}

	cmd = iscsit_allocate_cmd(conn, GFP_KERNEL);
	if (!cmd)
		return iscsit_add_reject(ISCSI_REASON_BOOKMARK_NO_RESOURCES, 1,
					buf, conn);

	cmd->iscsi_opcode       = ISCSI_OP_LOGOUT;
	cmd->i_state            = ISTATE_SEND_LOGOUTRSP;
	cmd->immediate_cmd      = ((hdr->opcode & ISCSI_OP_IMMEDIATE) ? 1 : 0);
	conn->sess->init_task_tag = cmd->init_task_tag  = hdr->itt;
	cmd->targ_xfer_tag      = 0xFFFFFFFF;
	cmd->cmd_sn             = hdr->cmdsn;
	cmd->exp_stat_sn        = hdr->exp_statsn;
	cmd->logout_cid         = hdr->cid;
	cmd->logout_reason      = reason_code;
	cmd->data_direction     = DMA_NONE;

	/*
	 * We need to sleep in these cases (by returning 1) until the Logout
	 * Response gets sent in the tx thread.
	 */
	if ((reason_code == ISCSI_LOGOUT_REASON_CLOSE_SESSION) ||
	   ((reason_code == ISCSI_LOGOUT_REASON_CLOSE_CONNECTION) &&
	    (hdr->cid == conn->cid)))
		logout_remove = 1;

	spin_lock_bh(&conn->cmd_lock);
	list_add_tail(&cmd->i_list, &conn->conn_cmd_list);
	spin_unlock_bh(&conn->cmd_lock);

	if (reason_code != ISCSI_LOGOUT_REASON_RECOVERY)
		iscsit_ack_from_expstatsn(conn, hdr->exp_statsn);

	/*
	 * Non-Immediate Logout Commands are executed in CmdSN order..
	 */
	if (!(hdr->opcode & ISCSI_OP_IMMEDIATE)) {
		cmdsn_ret = iscsit_check_received_cmdsn(conn, cmd, hdr->cmdsn);
		if ((cmdsn_ret == CMDSN_NORMAL_OPERATION) ||
		    (cmdsn_ret == CMDSN_HIGHER_THAN_EXP))
			return logout_remove;
		else if (cmdsn_ret == CMDSN_LOWER_THAN_EXP) {
			cmd->i_state = ISTATE_REMOVE;
			iscsit_add_cmd_to_immediate_queue(cmd, conn,
					cmd->i_state);
			return 0;
		} else { /* (cmdsn_ret == CMDSN_ERROR_CANNOT_RECOVER) */
			return iscsit_add_reject_from_cmd(
					ISCSI_REASON_PROTOCOL_ERROR,
					1, 0, buf, cmd);
		}
	}
	/*
	 * Immediate Logout Commands are executed, well, Immediately.
	 */
	if (iscsit_execute_cmd(cmd, 0) < 0)
		return -1;

	return logout_remove;
}

static int iscsit_handle_snack(
	struct iscsi_conn *conn,
	unsigned char *buf)
{
	u32 debug_type, unpacked_lun;
	u64 lun;
	struct iscsi_snack *hdr;

	hdr			= (struct iscsi_snack *) buf;
	hdr->flags		&= ~ISCSI_FLAG_CMD_FINAL;
	lun			= get_unaligned_le64(&hdr->lun[0]);
	unpacked_lun		= scsilun_to_int((struct scsi_lun *)&lun);
	hdr->itt		= be32_to_cpu(hdr->itt);
	hdr->ttt		= be32_to_cpu(hdr->ttt);
	hdr->exp_statsn		= be32_to_cpu(hdr->exp_statsn);
	hdr->begrun		= be32_to_cpu(hdr->begrun);
	hdr->runlength		= be32_to_cpu(hdr->runlength);

	debug_type = (hdr->flags & 0x02) ? TRACE_ISCSI : TRACE_ERL1;
	TRACE(debug_type, "Got ISCSI_INIT_SNACK, ITT: 0x%08x, ExpStatSN:"
		" 0x%08x, Type: 0x%02x, BegRun: 0x%08x, RunLength: 0x%08x,"
		" CID: %hu\n", hdr->itt, hdr->exp_statsn, hdr->flags,
			hdr->begrun, hdr->runlength, conn->cid);

	if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
		printk(KERN_ERR "Initiator sent SNACK request while in"
			" ErrorRecoveryLevel=0.\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}
	/*
	 * SNACK_DATA and SNACK_R2T are both 0,  so check which function to
	 * call from inside iscsi_send_recovery_datain_or_r2t().
	 */
	switch (hdr->flags & ISCSI_FLAG_SNACK_TYPE_MASK) {
	case 0:
		return iscsit_handle_recovery_datain_or_r2t(conn, buf,
			hdr->itt, hdr->ttt, hdr->begrun, hdr->runlength);
		return 0;
	case ISCSI_FLAG_SNACK_TYPE_STATUS:
		return iscsit_handle_status_snack(conn, hdr->itt, hdr->ttt,
			hdr->begrun, hdr->runlength);
	case ISCSI_FLAG_SNACK_TYPE_DATA_ACK:
		return iscsit_handle_data_ack(conn, hdr->ttt, hdr->begrun,
			hdr->runlength);
	case ISCSI_FLAG_SNACK_TYPE_RDATA:
		/* FIXME: Support R-Data SNACK */
		printk(KERN_ERR "R-Data SNACK Not Supported.\n");
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	default:
		printk(KERN_ERR "Unknown SNACK type 0x%02x, protocol"
			" error.\n", hdr->flags & 0x0f);
		return iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buf, conn);
	}

	return 0;
}

static void iscsit_rx_thread_wait_for_tcp(struct iscsi_conn *conn)
{
	if ((conn->sock->sk->sk_shutdown & SEND_SHUTDOWN) ||
	    (conn->sock->sk->sk_shutdown & RCV_SHUTDOWN)) {
		wait_for_completion_interruptible_timeout(
					&conn->rx_half_close_comp,
					ISCSI_RX_THREAD_TCP_TIMEOUT * HZ);
	}
}

static int iscsit_handle_immediate_data(
	struct iscsi_cmd *cmd,
	unsigned char *buf,
	u32 length)
{
	int iov_ret, rx_got = 0, rx_size = 0;
	u32 checksum, iov_count = 0, padding = 0, pad_bytes = 0;
	struct iscsi_conn *conn = cmd->conn;
	struct se_map_sg map_sg;
	struct se_unmap_sg unmap_sg;
	struct kvec *iov;

	memset(&map_sg, 0, sizeof(struct se_map_sg));
	memset(&unmap_sg, 0, sizeof(struct se_unmap_sg));
	map_sg.fabric_cmd = (void *)cmd;
	map_sg.se_cmd = SE_CMD(cmd);
	map_sg.sg_kmap_active = 1;
	map_sg.iov = &cmd->iov_data[0];
	map_sg.data_length = length;
	map_sg.data_offset = cmd->write_data_done;
	unmap_sg.fabric_cmd = (void *)cmd;
	unmap_sg.se_cmd = SE_CMD(cmd);

	iov_ret = iscsit_set_iovec_ptrs(&map_sg, &unmap_sg);
	if (iov_ret < 0)
		return IMMEDIATE_DATA_CANNOT_RECOVER;

	rx_size = length;
	iov_count = iov_ret;
	iov = &cmd->iov_data[0];

	padding = ((-length) & 3);
	if (padding != 0) {
		iov[iov_count].iov_base	= &pad_bytes;
		iov[iov_count++].iov_len = padding;
		rx_size += padding;
	}

	if (conn->conn_ops->DataDigest) {
		iov[iov_count].iov_base		= &checksum;
		iov[iov_count++].iov_len	= ISCSI_CRC_LEN;
		rx_size += ISCSI_CRC_LEN;
	}

	iscsit_map_SG_segments(&unmap_sg);

	rx_got = rx_data(conn, &cmd->iov_data[0], iov_count, rx_size);

	iscsit_unmap_SG_segments(&unmap_sg);

	if (rx_got != rx_size) {
		iscsit_rx_thread_wait_for_tcp(conn);
		return IMMEDIATE_DATA_CANNOT_RECOVER;
	}

	if (conn->conn_ops->DataDigest) {
		u32 counter = length, data_crc;
		struct kvec *iov_ptr = &cmd->iov_data[0];
		/*
		 * Thanks to the IP stack shitting on passed iovecs,  we have to
		 * call set_iovec_data_ptrs again in order to have a iMD/PSCSI
		 * agnostic way of doing datadigests computations.
		 */
		memset(&map_sg, 0, sizeof(struct se_map_sg));
		map_sg.fabric_cmd = (void *)cmd;
		map_sg.se_cmd = SE_CMD(cmd);
		map_sg.iov = iov_ptr;
		map_sg.data_length = length;
		map_sg.data_offset = cmd->write_data_done;

		if (iscsit_set_iovec_ptrs(&map_sg, &unmap_sg) < 0)
			return IMMEDIATE_DATA_CANNOT_RECOVER;

		iscsit_do_crypto_hash_iovec(&conn->conn_rx_hash, iov_ptr,
					counter, padding,
					(u8 *)&pad_bytes, (u8 *)&data_crc);

		if (checksum != data_crc) {
			printk(KERN_ERR "ImmediateData CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n", checksum,
				data_crc);

			if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
				printk(KERN_ERR "Unable to recover from"
					" Immediate Data digest failure while"
					" in ERL=0.\n");
				iscsit_add_reject_from_cmd(
						ISCSI_REASON_DATA_DIGEST_ERROR,
						1, 0, buf, cmd);
				return IMMEDIATE_DATA_CANNOT_RECOVER;
			} else {
				iscsit_add_reject_from_cmd(
						ISCSI_REASON_DATA_DIGEST_ERROR,
						0, 0, buf, cmd);
				return IMMEDIATE_DATA_ERL1_CRC_FAILURE;
			}
		} else {
			TRACE(TRACE_DIGEST, "Got CRC32C DataDigest 0x%08x for"
				" %u bytes of Immediate Data\n", checksum,
				length);
		}
	}

	cmd->write_data_done += length;

	if (cmd->write_data_done == cmd->data_length) {
		spin_lock_bh(&cmd->istate_lock);
		cmd->cmd_flags |= ICF_GOT_LAST_DATAOUT;
		cmd->i_state = ISTATE_RECEIVED_LAST_DATAOUT;
		spin_unlock_bh(&cmd->istate_lock);
	}

	return IMMEDIATE_DATA_NORMAL_OPERATION;
}

int iscsit_send_async_msg(
	struct iscsi_conn *conn,
	u16 cid,
	u8 async_event,
	u8 async_vcode)
{
	u8 iscsi_hdr[ISCSI_HDR_LEN+ISCSI_CRC_LEN];
	u32 tx_send = ISCSI_HDR_LEN, tx_sent = 0;
	struct iscsi_async *hdr;
	struct kvec iov;

	memset(&iov, 0, sizeof(struct kvec));
	memset(&iscsi_hdr, 0, ISCSI_HDR_LEN+ISCSI_CRC_LEN);

	hdr		= (struct iscsi_async *)&iscsi_hdr;
	hdr->opcode	= ISCSI_OP_ASYNC_EVENT;
	hdr->flags	|= ISCSI_FLAG_CMD_FINAL;
	hton24(hdr->dlength, 0);
	put_unaligned_le64(0, &hdr->lun[0]);
	put_unaligned_be64(0xffffffffffffffff, &hdr->rsvd4[0]);
	hdr->statsn	= cpu_to_be32(conn->stat_sn++);
	spin_lock(&conn->sess->cmdsn_lock);
	hdr->exp_cmdsn	= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn	= cpu_to_be32(conn->sess->max_cmd_sn);
	spin_unlock(&conn->sess->cmdsn_lock);
	hdr->async_event = async_event;
	hdr->async_vcode = async_vcode;

	switch (async_event) {
	case ISCSI_ASYNC_MSG_SCSI_EVENT:
		printk(KERN_ERR "ISCSI_ASYNC_MSG_SCSI_EVENT: not supported yet.\n");
		return -1;
	case ISCSI_ASYNC_MSG_REQUEST_LOGOUT:
		TRACE(TRACE_STATE, "Moving to"
				" TARG_CONN_STATE_LOGOUT_REQUESTED.\n");
		conn->conn_state = TARG_CONN_STATE_LOGOUT_REQUESTED;
		hdr->param1 = 0;
		hdr->param2 = 0;
		hdr->param3 = cpu_to_be16(SECONDS_FOR_ASYNC_LOGOUT);
		break;
	case ISCSI_ASYNC_MSG_DROPPING_CONNECTION:
		hdr->param1 = cpu_to_be16(cid);
		hdr->param2 = cpu_to_be16(conn->sess->sess_ops->DefaultTime2Wait);
		hdr->param3 = cpu_to_be16(conn->sess->sess_ops->DefaultTime2Retain);
		break;
	case ISCSI_ASYNC_MSG_DROPPING_ALL_CONNECTIONS:
		hdr->param1 = 0;
		hdr->param2 = cpu_to_be16(conn->sess->sess_ops->DefaultTime2Wait);
		hdr->param3 = cpu_to_be16(conn->sess->sess_ops->DefaultTime2Retain);
		break;
	case ISCSI_ASYNC_MSG_PARAM_NEGOTIATION:
		hdr->param1 = 0;
		hdr->param2 = 0;
		hdr->param3 = cpu_to_be16(SECONDS_FOR_ASYNC_TEXT);
		break;
	case ISCSI_ASYNC_MSG_VENDOR_SPECIFIC:
		printk(KERN_ERR "ISCSI_ASYNC_MSG_VENDOR_SPECIFIC not"
			" supported yet.\n");
		return -1;
	default:
		printk(KERN_ERR "Unknown AsycnEvent 0x%02x, protocol"
			" error.\n", async_event);
		return -1;
	}

	iov.iov_base	= &iscsi_hdr;
	iov.iov_len	= ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&iscsi_hdr[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)&iscsi_hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);
				
		iov.iov_len += ISCSI_CRC_LEN;
		tx_send += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for Async"
			" Msg PDU 0x%08x\n", *header_digest);
	}

	TRACE(TRACE_ISCSI, "Built Async Message StatSN: 0x%08x, AsyncEvent:"
		" 0x%02x, P1: 0x%04x, P2: 0x%04x, P3: 0x%04x\n",
		ntohl(hdr->statsn), hdr->async_event, ntohs(hdr->param1),
		ntohs(hdr->param2), ntohs(hdr->param3));

	tx_sent = tx_data(conn, &iov, 1, tx_send);
	if (tx_sent != tx_send) {
		printk(KERN_ERR "tx_data returned %d expecting %d\n",
				tx_sent, tx_send);
		return -1;
	}

	if (async_event == ISCSI_ASYNC_MSG_REQUEST_LOGOUT) {
		wait_for_completion_timeout(&conn->sess->async_msg_comp,
					SECONDS_FOR_ASYNC_LOGOUT * HZ);

		if (conn->conn_state == TARG_CONN_STATE_LOGOUT_REQUESTED) {
			printk(KERN_ERR "Asynchronous message timer expired"
				" without receiving a logout request,  dropping"
				" iSCSI session.\n");
			iscsit_send_async_msg(conn, 0,
				ISCSI_ASYNC_MSG_DROPPING_ALL_CONNECTIONS, 0);
			iscsit_free_session(conn->sess);
		}
	}
	return 0;
}

/*
 *	Called with sess->conn_lock held.
 */
/* #warning iscsi_build_conn_drop_async_message() only sends out on connections
	with active network interface */
static void iscsit_build_conn_drop_async_message(struct iscsi_conn *conn)
{
	struct iscsi_cmd *cmd;
	struct iscsi_conn *conn_p;

	/*
	 * Only send a Asynchronous Message on connections whos network
	 * interface is still functional.
	 */
	list_for_each_entry(conn_p, &conn->sess->sess_conn_list, conn_list) {
		if (conn_p->conn_state == TARG_CONN_STATE_LOGGED_IN) {
			iscsit_inc_conn_usage_count(conn_p);
			break;
		}
	}

	if (!conn_p)
		return;

	cmd = iscsit_allocate_cmd(conn_p, GFP_KERNEL);
	if (!cmd) {
		iscsit_dec_conn_usage_count(conn_p);
		return;
	}

	cmd->logout_cid = conn->cid;
	cmd->iscsi_opcode = ISCSI_OP_ASYNC_EVENT;
	cmd->i_state = ISTATE_SEND_ASYNCMSG;

	spin_lock_bh(&conn_p->cmd_lock);
	list_add_tail(&cmd->i_list, &conn_p->conn_cmd_list);
	spin_unlock_bh(&conn_p->cmd_lock);

	iscsit_add_cmd_to_response_queue(cmd, conn_p, cmd->i_state);
	iscsit_dec_conn_usage_count(conn_p);
}

static int iscsit_send_conn_drop_async_message(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	struct iscsi_async *hdr;

	cmd->tx_size = ISCSI_HDR_LEN;
	cmd->iscsi_opcode = ISCSI_OP_ASYNC_EVENT;

	hdr			= (struct iscsi_async *) cmd->pdu;
	hdr->opcode		= ISCSI_OP_ASYNC_EVENT;
	hdr->flags		= ISCSI_FLAG_CMD_FINAL;
	cmd->init_task_tag	= 0xFFFFFFFF;
	cmd->targ_xfer_tag	= 0xFFFFFFFF;
	put_unaligned_be64(0xffffffffffffffff, &hdr->rsvd4[0]);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);
	hdr->async_event	= ISCSI_ASYNC_MSG_DROPPING_CONNECTION;
	hdr->param1		= cpu_to_be16(cmd->logout_cid);
	hdr->param2		= cpu_to_be16(conn->sess->sess_ops->DefaultTime2Wait);
	hdr->param3		= cpu_to_be16(conn->sess->sess_ops->DefaultTime2Retain);

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		cmd->tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32C HeaderDigest to"
			" Async Message 0x%08x\n", *header_digest);
	}

	cmd->iov_misc[0].iov_base	= cmd->pdu;
	cmd->iov_misc[0].iov_len	= cmd->tx_size;
	cmd->iov_misc_count		= 1;

	TRACE(TRACE_ERL2, "Sending Connection Dropped Async Message StatSN:"
		" 0x%08x, for CID: %hu on CID: %hu\n", cmd->stat_sn,
			cmd->logout_cid, conn->cid);
	return 0;
}

static int iscsit_send_data_in(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn,
	struct se_unmap_sg *unmap_sg,
	int *eodr)
{
	int iov_ret = 0, set_statsn = 0;
	u8 *pad_bytes;
	u32 iov_count = 0, tx_size = 0;
	struct iscsi_datain datain;
	struct iscsi_datain_req *dr;
	struct se_map_sg map_sg;
	struct iscsi_data_rsp *hdr;
	struct kvec *iov;

	memset(&datain, 0, sizeof(struct iscsi_datain));
	dr = iscsit_get_datain_values(cmd, &datain);
	if (!dr) {
		printk(KERN_ERR "iscsit_get_datain_values failed for ITT: 0x%08x\n",
				cmd->init_task_tag);
		return -1;
	}

	/*
	 * Be paranoid and double check the logic for now.
	 */
	if ((datain.offset + datain.length) > cmd->data_length) {
		printk(KERN_ERR "Command ITT: 0x%08x, datain.offset: %u and"
			" datain.length: %u exceeds cmd->data_length: %u\n",
			cmd->init_task_tag, datain.offset, datain.length,
				cmd->data_length);
		return -1;
	}

	spin_lock_bh(&conn->sess->session_stats_lock);
	conn->sess->tx_data_octets += datain.length;
	if (conn->sess->se_sess->se_node_acl) {
		spin_lock(&conn->sess->se_sess->se_node_acl->stats_lock);
		conn->sess->se_sess->se_node_acl->read_bytes += datain.length;
		spin_unlock(&conn->sess->se_sess->se_node_acl->stats_lock);
	}
	spin_unlock_bh(&conn->sess->session_stats_lock);
	/*
	 * Special case for successfully execution w/ both DATAIN
	 * and Sense Data.
	 */
	if ((datain.flags & ISCSI_FLAG_DATA_STATUS) &&
	    (SE_CMD(cmd)->se_cmd_flags & SCF_TRANSPORT_TASK_SENSE))
		datain.flags &= ~ISCSI_FLAG_DATA_STATUS;
	else {
		if ((dr->dr_complete == DATAIN_COMPLETE_NORMAL) ||
		    (dr->dr_complete == DATAIN_COMPLETE_CONNECTION_RECOVERY)) {
			iscsit_increment_maxcmdsn(cmd, conn->sess);
			cmd->stat_sn = conn->stat_sn++;
			set_statsn = 1;
		} else if (dr->dr_complete ==
				DATAIN_COMPLETE_WITHIN_COMMAND_RECOVERY)
			set_statsn = 1;
	}

	hdr	= (struct iscsi_data_rsp *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_SCSI_DATA_IN;
	hdr->flags		= datain.flags;
	if (hdr->flags & ISCSI_FLAG_DATA_STATUS) {
		if (SE_CMD(cmd)->se_cmd_flags & SCF_OVERFLOW_BIT) {
			hdr->flags |= ISCSI_FLAG_DATA_OVERFLOW;
			hdr->residual_count = cpu_to_be32(cmd->residual_count);
		} else if (SE_CMD(cmd)->se_cmd_flags & SCF_UNDERFLOW_BIT) {
			hdr->flags |= ISCSI_FLAG_DATA_UNDERFLOW;
			hdr->residual_count = cpu_to_be32(cmd->residual_count);
		}
	}
	hton24(hdr->dlength, datain.length);
	if (hdr->flags & ISCSI_FLAG_DATA_ACK)
		int_to_scsilun(SE_CMD(cmd)->orig_fe_lun,
				(struct scsi_lun *)&hdr->lun[0]);
	else
		put_unaligned_le64(0xFFFFFFFFFFFFFFFFULL, &hdr->lun[0]);

	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	hdr->ttt		= (hdr->flags & ISCSI_FLAG_DATA_ACK) ?
				   cpu_to_be32(cmd->targ_xfer_tag) :
				   0xFFFFFFFF;
	hdr->statsn		= (set_statsn) ? cpu_to_be32(cmd->stat_sn) :
						0xFFFFFFFF;
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);
	hdr->datasn		= cpu_to_be32(datain.data_sn);
	hdr->offset		= cpu_to_be32(datain.offset);

	iov = &cmd->iov_data[0];
	iov[iov_count].iov_base	= cmd->pdu;
	iov[iov_count++].iov_len	= ISCSI_HDR_LEN;
	tx_size += ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);	

		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;

		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest"
			" for DataIN PDU 0x%08x\n", *header_digest);
	}

	memset(&map_sg, 0, sizeof(struct se_map_sg));
	map_sg.fabric_cmd = (void *)cmd;
	map_sg.se_cmd = SE_CMD(cmd);
	map_sg.sg_kmap_active = 1;
	map_sg.iov = &cmd->iov_data[1];
	map_sg.data_length = datain.length;
	map_sg.data_offset = datain.offset;

	iov_ret = iscsit_set_iovec_ptrs(&map_sg, unmap_sg);
	if (iov_ret < 0)
		return -1;

	iov_count += iov_ret;
	tx_size += datain.length;

	unmap_sg->padding = ((-datain.length) & 3);
	if (unmap_sg->padding != 0) {
		pad_bytes = kzalloc(unmap_sg->padding * sizeof(u8),
					GFP_KERNEL);
		if (!pad_bytes) {
			printk(KERN_ERR "Unable to allocate memory for"
					" pad_bytes.\n");
			return -1;
		}
		cmd->buf_ptr = pad_bytes;
		iov[iov_count].iov_base		= pad_bytes;
		iov[iov_count++].iov_len	= unmap_sg->padding;
		tx_size += unmap_sg->padding;

		TRACE(TRACE_ISCSI, "Attaching %u padding bytes\n",
				unmap_sg->padding);
	}
	if (conn->conn_ops->DataDigest) {
		u32 counter = (datain.length + unmap_sg->padding);
		struct kvec *iov_ptr = &cmd->iov_data[1];

		iscsit_do_crypto_hash_iovec(&conn->conn_tx_hash, iov_ptr,
				counter, 0, NULL, (u8 *)&cmd->data_crc);

		iov[iov_count].iov_base	= &cmd->data_crc;
		iov[iov_count++].iov_len = ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;

		TRACE(TRACE_DIGEST, "Attached CRC32C DataDigest %d bytes, crc"
			" 0x%08x\n", datain.length+unmap_sg->padding,
			cmd->data_crc);
	}

	cmd->iov_data_count = iov_count;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ISCSI, "Built DataIN ITT: 0x%08x, StatSN: 0x%08x,"
		" DataSN: 0x%08x, Offset: %u, Length: %u, CID: %hu\n",
		cmd->init_task_tag, ntohl(hdr->statsn), ntohl(hdr->datasn),
		ntohl(hdr->offset), datain.length, conn->cid);

	if (dr->dr_complete) {
		*eodr = (SE_CMD(cmd)->se_cmd_flags & SCF_TRANSPORT_TASK_SENSE) ?
				2 : 1;
		iscsit_free_datain_req(cmd, dr);
	}

	return 0;
}

static int iscsit_send_logout_response(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	int niov = 0, tx_size;
	struct iscsi_conn *logout_conn = NULL;
	struct iscsi_conn_recovery *cr = NULL;
	struct iscsi_session *sess = conn->sess;
	struct kvec *iov;
	struct iscsi_logout_rsp *hdr;
	/*
	 * The actual shutting down of Sessions and/or Connections
	 * for CLOSESESSION and CLOSECONNECTION Logout Requests
	 * is done in scsi_logout_post_handler().
	 */
	switch (cmd->logout_reason) {
	case ISCSI_LOGOUT_REASON_CLOSE_SESSION:
		TRACE(TRACE_ISCSI, "iSCSI session logout successful, setting"
			" logout response to ISCSI_LOGOUT_SUCCESS.\n");
		cmd->logout_response = ISCSI_LOGOUT_SUCCESS;
		break;
	case ISCSI_LOGOUT_REASON_CLOSE_CONNECTION:
		if (cmd->logout_response == ISCSI_LOGOUT_CID_NOT_FOUND)
			break;
		/*
		 * For CLOSECONNECTION logout requests carrying
		 * a matching logout CID -> local CID, the reference
		 * for the local CID will have been incremented in
		 * iscsi_logout_closeconnection().
		 *
		 * For CLOSECONNECTION logout requests carrying
		 * a different CID than the connection it arrived
		 * on, the connection responding to cmd->logout_cid
		 * is stopped in iscsit_logout_post_handler_diffcid().
		 */

		TRACE(TRACE_ISCSI, "iSCSI CID: %hu logout on CID: %hu"
			" successful.\n", cmd->logout_cid, conn->cid);
		cmd->logout_response = ISCSI_LOGOUT_SUCCESS;
		break;
	case ISCSI_LOGOUT_REASON_RECOVERY:
		if ((cmd->logout_response == ISCSI_LOGOUT_RECOVERY_UNSUPPORTED) ||
		    (cmd->logout_response == ISCSI_LOGOUT_CLEANUP_FAILED))
			break;
		/*
		 * If the connection is still active from our point of view
		 * force connection recovery to occur.
		 */
		logout_conn = iscsit_get_conn_from_cid_rcfr(sess,
				cmd->logout_cid);
		if ((logout_conn)) {
			iscsit_connection_reinstatement_rcfr(logout_conn);
			iscsit_dec_conn_usage_count(logout_conn);
		}

		cr = iscsit_get_inactive_connection_recovery_entry(
				conn->sess, cmd->logout_cid);
		if (!cr) {
			printk(KERN_ERR "Unable to locate CID: %hu for"
			" REMOVECONNFORRECOVERY Logout Request.\n",
				cmd->logout_cid);
			cmd->logout_response = ISCSI_LOGOUT_CID_NOT_FOUND;
			break;
		}

		iscsit_discard_cr_cmds_by_expstatsn(cr, cmd->exp_stat_sn);

		TRACE(TRACE_ERL2, "iSCSI REMOVECONNFORRECOVERY logout"
			" for recovery for CID: %hu on CID: %hu successful.\n",
				cmd->logout_cid, conn->cid);
		cmd->logout_response = ISCSI_LOGOUT_SUCCESS;
		break;
	default:
		printk(KERN_ERR "Unknown cmd->logout_reason: 0x%02x\n",
				cmd->logout_reason);
		return -1;
	}

	tx_size = ISCSI_HDR_LEN;
	hdr			= (struct iscsi_logout_rsp *)cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_LOGOUT_RSP;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	hdr->response		= cmd->logout_response;
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);

	iscsit_increment_maxcmdsn(cmd, conn->sess);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	iov = &cmd->iov_misc[0];
	iov[niov].iov_base	= cmd->pdu;
	iov[niov++].iov_len	= ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32C HeaderDigest to"
			" Logout Response 0x%08x\n", *header_digest);
	}
	cmd->iov_misc_count = niov;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ISCSI, "Sending Logout Response ITT: 0x%08x StatSN:"
		" 0x%08x Response: 0x%02x CID: %hu on CID: %hu\n",
		cmd->init_task_tag, cmd->stat_sn, hdr->response,
		cmd->logout_cid, conn->cid);

	return 0;
}

/*
 *	Unsolicited NOPIN, either requesting a response or not.
 */
static int iscsit_send_unsolicited_nopin(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn,
	int want_response)
{
	int tx_size = ISCSI_HDR_LEN;
	struct iscsi_nopin *hdr;

	hdr			= (struct iscsi_nopin *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_NOOP_IN;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	hdr->ttt		= cpu_to_be32(cmd->targ_xfer_tag);
	cmd->stat_sn		= conn->stat_sn;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32C HeaderDigest to"
			" NopIN 0x%08x\n", *header_digest);
	}

	cmd->iov_misc[0].iov_base	= cmd->pdu;
	cmd->iov_misc[0].iov_len	= tx_size;
	cmd->iov_misc_count	= 1;
	cmd->tx_size		= tx_size;

	TRACE(TRACE_ISCSI, "Sending Unsolicited NOPIN TTT: 0x%08x StatSN:"
		" 0x%08x CID: %hu\n", hdr->ttt, cmd->stat_sn, conn->cid);

	return 0;
}

static int iscsit_send_nopin_response(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	int niov = 0, tx_size;
	u32 padding = 0;
	struct kvec *iov;
	struct iscsi_nopin *hdr;

	tx_size = ISCSI_HDR_LEN;
	hdr			= (struct iscsi_nopin *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_NOOP_IN;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	hton24(hdr->dlength, cmd->buf_ptr_size);
	put_unaligned_le64(0xFFFFFFFFFFFFFFFFULL, &hdr->lun[0]);
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	hdr->ttt		= cpu_to_be32(cmd->targ_xfer_tag);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);

	iscsit_increment_maxcmdsn(cmd, conn->sess);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	iov = &cmd->iov_misc[0];
	iov[niov].iov_base	= cmd->pdu;
	iov[niov++].iov_len	= ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);
					
		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32C HeaderDigest"
			" to NopIn 0x%08x\n", *header_digest);
	}

	/*
	 * NOPOUT Ping Data is attached to struct iscsi_cmd->buf_ptr.
	 * NOPOUT DataSegmentLength is at struct iscsi_cmd->buf_ptr_size.
	 */
	if (cmd->buf_ptr_size) {
		iov[niov].iov_base	= cmd->buf_ptr;
		iov[niov++].iov_len	= cmd->buf_ptr_size;
		tx_size += cmd->buf_ptr_size;

		TRACE(TRACE_ISCSI, "Echoing back %u bytes of ping"
			" data.\n", cmd->buf_ptr_size);

		padding = ((-cmd->buf_ptr_size) & 3);
		if (padding != 0) {
			iov[niov].iov_base = &cmd->pad_bytes;
			iov[niov++].iov_len = padding;
			tx_size += padding;
			TRACE(TRACE_ISCSI, "Attaching %u additional"
				" padding bytes.\n", padding);
		}
		if (conn->conn_ops->DataDigest) {
			iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				cmd->buf_ptr, cmd->buf_ptr_size,
				padding, (u8 *)&cmd->pad_bytes,
				(u8 *)&cmd->data_crc);
			
			iov[niov].iov_base = &cmd->data_crc;
			iov[niov++].iov_len = ISCSI_CRC_LEN;
			tx_size += ISCSI_CRC_LEN;
			TRACE(TRACE_DIGEST, "Attached DataDigest for %u"
				" bytes of ping data, CRC 0x%08x\n",
				cmd->buf_ptr_size, cmd->data_crc);
		}
	}

	cmd->iov_misc_count = niov;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ISCSI, "Sending NOPIN Response ITT: 0x%08x, TTT:"
		" 0x%08x, StatSN: 0x%08x, Length %u\n", cmd->init_task_tag,
		cmd->targ_xfer_tag, cmd->stat_sn, cmd->buf_ptr_size);

	return 0;
}

int iscsit_send_r2t(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	int tx_size = 0;
	u32 trace_type;
	struct iscsi_r2t *r2t;
	struct iscsi_r2t_rsp *hdr;

	r2t = iscsit_get_r2t_from_list(cmd);
	if (!r2t)
		return -1;

	hdr			= (struct iscsi_r2t_rsp *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_R2T;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	int_to_scsilun(SE_CMD(cmd)->orig_fe_lun,
			(struct scsi_lun *)&hdr->lun[0]);
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	spin_lock_bh(&conn->sess->ttt_lock);
	r2t->targ_xfer_tag	= conn->sess->targ_xfer_tag++;
	if (r2t->targ_xfer_tag == 0xFFFFFFFF)
		r2t->targ_xfer_tag = conn->sess->targ_xfer_tag++;
	spin_unlock_bh(&conn->sess->ttt_lock);
	hdr->ttt		= cpu_to_be32(r2t->targ_xfer_tag);
	hdr->statsn		= cpu_to_be32(conn->stat_sn);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);
	hdr->r2tsn		= cpu_to_be32(r2t->r2t_sn);
	hdr->data_offset	= cpu_to_be32(r2t->offset);
	hdr->data_length	= cpu_to_be32(r2t->xfer_len);

	cmd->iov_misc[0].iov_base	= cmd->pdu;
	cmd->iov_misc[0].iov_len	= ISCSI_HDR_LEN;
	tx_size += ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		cmd->iov_misc[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for R2T"
			" PDU 0x%08x\n", *header_digest);
	}

	trace_type = (!r2t->recovery_r2t) ? TRACE_ISCSI : TRACE_ERL1;
	TRACE(trace_type, "Built %sR2T, ITT: 0x%08x, TTT: 0x%08x, StatSN:"
		" 0x%08x, R2TSN: 0x%08x, Offset: %u, DDTL: %u, CID: %hu\n",
		(!r2t->recovery_r2t) ? "" : "Recovery ", cmd->init_task_tag,
		r2t->targ_xfer_tag, ntohl(hdr->statsn), r2t->r2t_sn,
			r2t->offset, r2t->xfer_len, conn->cid);

	cmd->iov_misc_count = 1;
	cmd->tx_size = tx_size;

	spin_lock_bh(&cmd->r2t_lock);
	r2t->sent_r2t = 1;
	spin_unlock_bh(&cmd->r2t_lock);

	return 0;
}

/*
 *	type 0: Normal Operation.
 *	type 1: Called from Storage Transport.
 *	type 2: Called from iscsi_task_reassign_complete_write() for
 *	        connection recovery.
 */
int iscsit_build_r2ts_for_cmd(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn,
	int type)
{
	int first_r2t = 1;
	u32 offset = 0, xfer_len = 0;

	spin_lock_bh(&cmd->r2t_lock);
	if (cmd->cmd_flags & ICF_SENT_LAST_R2T) {
		spin_unlock_bh(&cmd->r2t_lock);
		return 0;
	}

	if (conn->sess->sess_ops->DataSequenceInOrder && (type != 2))
		if (cmd->r2t_offset < cmd->write_data_done)
			cmd->r2t_offset = cmd->write_data_done;

	while (cmd->outstanding_r2ts < conn->sess->sess_ops->MaxOutstandingR2T) {
		if (conn->sess->sess_ops->DataSequenceInOrder) {
			offset = cmd->r2t_offset;

			if (first_r2t && (type == 2)) {
				xfer_len = ((offset +
					     (conn->sess->sess_ops->MaxBurstLength -
					     cmd->next_burst_len) >
					     cmd->data_length) ?
					    (cmd->data_length - offset) :
					    (conn->sess->sess_ops->MaxBurstLength -
					     cmd->next_burst_len));
			} else {
				xfer_len = ((offset +
					     conn->sess->sess_ops->MaxBurstLength) >
					     cmd->data_length) ?
					     (cmd->data_length - offset) :
					     conn->sess->sess_ops->MaxBurstLength;
			}
			cmd->r2t_offset += xfer_len;

			if (cmd->r2t_offset == cmd->data_length)
				cmd->cmd_flags |= ICF_SENT_LAST_R2T;
		} else {
			struct iscsi_seq *seq;

			seq = iscsit_get_seq_holder_for_r2t(cmd);
			if (!seq) {
				spin_unlock_bh(&cmd->r2t_lock);
				return -1;
			}

			offset = seq->offset;
			xfer_len = seq->xfer_len;

			if (cmd->seq_send_order == cmd->seq_count)
				cmd->cmd_flags |= ICF_SENT_LAST_R2T;
		}
		cmd->outstanding_r2ts++;
		first_r2t = 0;

		if (iscsit_add_r2t_to_list(cmd, offset, xfer_len, 0, 0) < 0) {
			spin_unlock_bh(&cmd->r2t_lock);
			return -1;
		}

		if (cmd->cmd_flags & ICF_SENT_LAST_R2T)
			break;
	}
	spin_unlock_bh(&cmd->r2t_lock);

	return 0;
}

static int iscsit_send_status(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	u8 iov_count = 0, recovery;
	u32 padding = 0, trace_type, tx_size = 0;
	struct iscsi_scsi_rsp *hdr;
	struct kvec *iov;

	recovery = (cmd->i_state != ISTATE_SEND_STATUS);
	if (!recovery)
		cmd->stat_sn = conn->stat_sn++;

	spin_lock_bh(&conn->sess->session_stats_lock);
	conn->sess->rsp_pdus++;
	spin_unlock_bh(&conn->sess->session_stats_lock);

	hdr			= (struct iscsi_scsi_rsp *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_SCSI_CMD_RSP;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	if (SE_CMD(cmd)->se_cmd_flags & SCF_OVERFLOW_BIT) {
		hdr->flags |= ISCSI_FLAG_CMD_OVERFLOW;
		hdr->residual_count = cpu_to_be32(cmd->residual_count);
	} else if (SE_CMD(cmd)->se_cmd_flags & SCF_UNDERFLOW_BIT) {
		hdr->flags |= ISCSI_FLAG_CMD_UNDERFLOW;
		hdr->residual_count = cpu_to_be32(cmd->residual_count);
	}
	hdr->response		= cmd->iscsi_response;
	hdr->cmd_status		= SE_CMD(cmd)->scsi_status;
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);

	iscsit_increment_maxcmdsn(cmd, conn->sess);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	iov = &cmd->iov_misc[0];
	iov[iov_count].iov_base	= cmd->pdu;
	iov[iov_count++].iov_len = ISCSI_HDR_LEN;
	tx_size += ISCSI_HDR_LEN;

	/*
	 * Attach SENSE DATA payload to iSCSI Response PDU
	 */
	if (SE_CMD(cmd)->sense_buffer &&
	   ((SE_CMD(cmd)->se_cmd_flags & SCF_TRANSPORT_TASK_SENSE) ||
	    (SE_CMD(cmd)->se_cmd_flags & SCF_EMULATED_TASK_SENSE))) {
		padding		= -(SE_CMD(cmd)->scsi_sense_length) & 3;
		hton24(hdr->dlength, SE_CMD(cmd)->scsi_sense_length);
		iov[iov_count].iov_base	= SE_CMD(cmd)->sense_buffer;
		iov[iov_count++].iov_len =
				(SE_CMD(cmd)->scsi_sense_length + padding);
		tx_size += SE_CMD(cmd)->scsi_sense_length;

		if (padding) {
			memset(SE_CMD(cmd)->sense_buffer +
				SE_CMD(cmd)->scsi_sense_length, 0, padding);
			tx_size += padding;
			TRACE(TRACE_ISCSI, "Adding %u bytes of padding to"
				" SENSE.\n", padding);
		}

		if (conn->conn_ops->DataDigest) {
			iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				SE_CMD(cmd)->sense_buffer,
				(SE_CMD(cmd)->scsi_sense_length + padding),
				0, NULL, (u8 *)&cmd->data_crc);

			iov[iov_count].iov_base    = &cmd->data_crc;
			iov[iov_count++].iov_len     = ISCSI_CRC_LEN;
			tx_size += ISCSI_CRC_LEN;

			TRACE(TRACE_DIGEST, "Attaching CRC32 DataDigest for"
				" SENSE, %u bytes CRC 0x%08x\n",
				(SE_CMD(cmd)->scsi_sense_length + padding),
				cmd->data_crc);
		}

		TRACE(TRACE_ISCSI, "Attaching SENSE DATA: %u bytes to iSCSI"
				" Response PDU\n",
				SE_CMD(cmd)->scsi_sense_length);
	}

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for Response"
				" PDU 0x%08x\n", *header_digest);
	}

	cmd->iov_misc_count = iov_count;
	cmd->tx_size = tx_size;

	trace_type = (!recovery) ? TRACE_ISCSI : TRACE_ERL1;
	TRACE(trace_type, "Built %sSCSI Response, ITT: 0x%08x, StatSN: 0x%08x,"
		" Response: 0x%02x, SAM Status: 0x%02x, CID: %hu\n",
		(!recovery) ? "" : "Recovery ", cmd->init_task_tag,
		cmd->stat_sn, 0x00, cmd->se_cmd.scsi_status, conn->cid);

	return 0;
}

static u8 iscsit_convert_tcm_tmr_rsp(struct se_tmr_req *se_tmr)
{
	switch (se_tmr->response) {
	case TMR_FUNCTION_COMPLETE:
		return ISCSI_TMF_RSP_COMPLETE;
	case TMR_TASK_DOES_NOT_EXIST:
		return ISCSI_TMF_RSP_NO_TASK;
	case TMR_LUN_DOES_NOT_EXIST:
		return ISCSI_TMF_RSP_NO_LUN;
	case TMR_TASK_MGMT_FUNCTION_NOT_SUPPORTED:
		return ISCSI_TMF_RSP_NOT_SUPPORTED;
	case TMR_FUNCTION_AUTHORIZATION_FAILED:
		return ISCSI_TMF_RSP_AUTH_FAILED;
	case TMR_FUNCTION_REJECTED:
	default:
		return ISCSI_TMF_RSP_REJECTED;
	}
}

static int iscsit_send_task_mgt_rsp(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	struct se_tmr_req *se_tmr = SE_CMD(cmd)->se_tmr_req;
	struct iscsi_tm_rsp *hdr;
	u32 tx_size = 0;

	hdr			= (struct iscsi_tm_rsp *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_SCSI_TMFUNC_RSP;
	hdr->response		= iscsit_convert_tcm_tmr_rsp(se_tmr);
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);

	iscsit_increment_maxcmdsn(cmd, conn->sess);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	cmd->iov_misc[0].iov_base	= cmd->pdu;
	cmd->iov_misc[0].iov_len	= ISCSI_HDR_LEN;
	tx_size += ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		cmd->iov_misc[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for Task"
			" Mgmt Response PDU 0x%08x\n", *header_digest);
	}

	cmd->iov_misc_count = 1;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ERL2, "Built Task Management Response ITT: 0x%08x,"
		" StatSN: 0x%08x, Response: 0x%02x, CID: %hu\n",
		cmd->init_task_tag, cmd->stat_sn, hdr->response, conn->cid);

	return 0;
}

static int iscsit_build_sendtargets_response(struct iscsi_cmd *cmd)
{
	char *payload = NULL;
	struct iscsi_conn *conn = cmd->conn;
	struct iscsi_portal_group *tpg;
	struct iscsi_tiqn *tiqn;
	struct iscsi_tpg_np *tpg_np;
	int buffer_len, end_of_buf = 0, len = 0, payload_len = 0;
	unsigned char buf[256];

	buffer_len = (conn->conn_ops->MaxRecvDataSegmentLength > 32768) ?
			32768 : conn->conn_ops->MaxRecvDataSegmentLength;

	memset(buf, 0, 256);

	payload = kzalloc(buffer_len, GFP_KERNEL);
	if (!payload) {
		printk(KERN_ERR "Unable to allocate memory for sendtargets"
				" response.\n");
		return -ENOMEM;
	}

	spin_lock(&tiqn_lock);
	list_for_each_entry(tiqn, &g_tiqn_list, tiqn_list) {
		len = sprintf(buf, "TargetName=%s", tiqn->tiqn);
		len += 1;

		if ((len + payload_len) > buffer_len) {
			spin_unlock(&tiqn->tiqn_tpg_lock);
			end_of_buf = 1;
			goto eob;
		}
		memcpy((void *)payload + payload_len, buf, len);
		payload_len += len;

		spin_lock(&tiqn->tiqn_tpg_lock);
		list_for_each_entry(tpg, &tiqn->tiqn_tpg_list, tpg_list) {

			spin_lock(&tpg->tpg_state_lock);
			if ((tpg->tpg_state == TPG_STATE_FREE) ||
			    (tpg->tpg_state == TPG_STATE_INACTIVE)) {
				spin_unlock(&tpg->tpg_state_lock);
				continue;
			}
			spin_unlock(&tpg->tpg_state_lock);

			spin_lock(&tpg->tpg_np_lock);
			list_for_each_entry(tpg_np, &tpg->tpg_gnp_list,
						tpg_np_list) {
				len = sprintf(buf, "TargetAddress="
					"%s%s%s:%hu,%hu",
					(tpg_np->tpg_np->np_sockaddr.ss_family == AF_INET6) ?
					"[" : "", tpg_np->tpg_np->np_ip,
					(tpg_np->tpg_np->np_sockaddr.ss_family == AF_INET6) ?
					"]" : "", tpg_np->tpg_np->np_port,
					tpg->tpgt);
				len += 1;

				if ((len + payload_len) > buffer_len) {
					spin_unlock(&tpg->tpg_np_lock);
					spin_unlock(&tiqn->tiqn_tpg_lock);
					end_of_buf = 1;
					goto eob;
				}
				memcpy((void *)payload + payload_len, buf, len);
				payload_len += len;
			}
			spin_unlock(&tpg->tpg_np_lock);
		}
		spin_unlock(&tiqn->tiqn_tpg_lock);
eob:
		if (end_of_buf)
			break;
	}
	spin_unlock(&tiqn_lock);

	cmd->buf_ptr = payload;

	return payload_len;
}

/*
 *	FIXME: Add support for F_BIT and C_BIT when the length is longer than
 *	MaxRecvDataSegmentLength.
 */
static int iscsit_send_text_rsp(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	struct iscsi_text_rsp *hdr;
	struct kvec *iov;
	u32 padding = 0, tx_size = 0;
	int text_length, iov_count = 0;

	text_length = iscsit_build_sendtargets_response(cmd);
	if (text_length < 0)
		return text_length;

	padding = ((-text_length) & 3);
	if (padding != 0) {
		memset(cmd->buf_ptr + text_length, 0, padding);
		TRACE(TRACE_ISCSI, "Attaching %u additional bytes for"
			" padding.\n", padding);
	}

	hdr			= (struct iscsi_text_rsp *) cmd->pdu;
	memset(hdr, 0, ISCSI_HDR_LEN);
	hdr->opcode		= ISCSI_OP_TEXT_RSP;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	hton24(hdr->dlength, text_length);
	hdr->itt		= cpu_to_be32(cmd->init_task_tag);
	hdr->ttt		= cpu_to_be32(cmd->targ_xfer_tag);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);

	iscsit_increment_maxcmdsn(cmd, conn->sess);
	hdr->exp_cmdsn		= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn		= cpu_to_be32(conn->sess->max_cmd_sn);

	iov = &cmd->iov_misc[0];

	iov[iov_count].iov_base = cmd->pdu;
	iov[iov_count++].iov_len = ISCSI_HDR_LEN;
	iov[iov_count].iov_base	= cmd->buf_ptr;
	iov[iov_count++].iov_len = text_length + padding;

	tx_size += (ISCSI_HDR_LEN + text_length + padding);

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for"
			" Text Response PDU 0x%08x\n", *header_digest);
	}

	if (conn->conn_ops->DataDigest) {
		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				cmd->buf_ptr, (text_length + padding),
				0, NULL, (u8 *)&cmd->data_crc);

		iov[iov_count].iov_base	= &cmd->data_crc;
		iov[iov_count++].iov_len = ISCSI_CRC_LEN;
		tx_size	+= ISCSI_CRC_LEN;

		TRACE(TRACE_DIGEST, "Attaching DataDigest for %u bytes of text"
			" data, CRC 0x%08x\n", (text_length + padding),
			cmd->data_crc);
	}

	cmd->iov_misc_count = iov_count;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ISCSI, "Built Text Response: ITT: 0x%08x, StatSN: 0x%08x,"
		" Length: %u, CID: %hu\n", cmd->init_task_tag, cmd->stat_sn,
			text_length, conn->cid);
	return 0;
}

static int iscsit_send_reject(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	u32 iov_count = 0, tx_size = 0;
	struct iscsi_reject *hdr;
	struct kvec *iov;

	hdr			= (struct iscsi_reject *) cmd->pdu;
	hdr->opcode		= ISCSI_OP_REJECT;
	hdr->flags		|= ISCSI_FLAG_CMD_FINAL;
	hton24(hdr->dlength, ISCSI_HDR_LEN);
	cmd->stat_sn		= conn->stat_sn++;
	hdr->statsn		= cpu_to_be32(cmd->stat_sn);
	hdr->exp_cmdsn	= cpu_to_be32(conn->sess->exp_cmd_sn);
	hdr->max_cmdsn	= cpu_to_be32(conn->sess->max_cmd_sn);

	iov = &cmd->iov_misc[0];

	iov[iov_count].iov_base = cmd->pdu;
	iov[iov_count++].iov_len = ISCSI_HDR_LEN;
	iov[iov_count].iov_base = cmd->buf_ptr;
	iov[iov_count++].iov_len = ISCSI_HDR_LEN;

	tx_size = (ISCSI_HDR_LEN + ISCSI_HDR_LEN);

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)hdr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)header_digest);

		iov[0].iov_len += ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 HeaderDigest for"
			" REJECT PDU 0x%08x\n", *header_digest);
	}

	if (conn->conn_ops->DataDigest) {
		iscsit_do_crypto_hash_buf(&conn->conn_tx_hash,
				(unsigned char *)cmd->buf_ptr, ISCSI_HDR_LEN,
				0, NULL, (u8 *)&cmd->data_crc);

		iov[iov_count].iov_base = &cmd->data_crc;
		iov[iov_count++].iov_len  = ISCSI_CRC_LEN;
		tx_size += ISCSI_CRC_LEN;
		TRACE(TRACE_DIGEST, "Attaching CRC32 DataDigest for REJECT"
				" PDU 0x%08x\n", cmd->data_crc);
	}

	cmd->iov_misc_count = iov_count;
	cmd->tx_size = tx_size;

	TRACE(TRACE_ISCSI, "Built Reject PDU StatSN: 0x%08x, Reason: 0x%02x,"
		" CID: %hu\n", ntohl(hdr->statsn), hdr->reason, conn->cid);

	return 0;
}

static void iscsit_tx_thread_wait_for_tcp(struct iscsi_conn *conn)
{
	if ((conn->sock->sk->sk_shutdown & SEND_SHUTDOWN) ||
	    (conn->sock->sk->sk_shutdown & RCV_SHUTDOWN)) {
		wait_for_completion_interruptible_timeout(
					&conn->tx_half_close_comp,
					ISCSI_TX_THREAD_TCP_TIMEOUT * HZ);
	}
}

#ifdef CONFIG_SMP

void iscsit_thread_get_cpumask(struct iscsi_conn *conn)
{
	struct iscsi_thread_set *ts = conn->thread_set;
	int ord, cpu;
	/*
	 * thread_id is assigned from iscsit_global->ts_bitmap from
	 * within iscsi_thread_set.c:iscsi_allocate_thread_sets()
	 *
	 * Here we use thread_id to determine which CPU that this
	 * iSCSI connection's iscsi_thread_set will be scheduled to
	 * execute upon.
	 */
	ord = ts->thread_id % cpumask_weight(cpu_online_mask);
#if 0
	printk(KERN_INFO ">>>>>>>>>>>>>>>>>>>> Generated ord: %d from"
			" thread_id: %d\n", ord, ts->thread_id);
#endif
	for_each_online_cpu(cpu) {
		if (ord-- == 0) {
			cpumask_set_cpu(cpu, conn->conn_cpumask);
			return;
		}
	}
	/*
	 * This should never be reached..
	 */
	dump_stack();
	cpumask_setall(conn->conn_cpumask);
}

static inline void iscsit_thread_check_cpumask(
	struct iscsi_conn *conn,
	struct task_struct *p,
	int mode)
{
	char buf[128];
	/*
	 * mode == 1 signals iscsi_target_tx_thread() usage.
	 * mode == 0 signals iscsi_target_rx_thread() usage.
	 */
	if (mode == 1) {
		if (!conn->conn_tx_reset_cpumask)
			return;
		conn->conn_tx_reset_cpumask = 0;
	} else {
		if (!conn->conn_rx_reset_cpumask)
			return;
		conn->conn_rx_reset_cpumask = 0;
	}
	/*
	 * Update the CPU mask for this single kthread so that
	 * both TX and RX kthreads are scheduled to run on the
	 * same CPU.
	 */
	memset(buf, 0, 128);
	cpumask_scnprintf(buf, 128, conn->conn_cpumask);
#if 0
	printk(KERN_INFO ">>>>>>>>>>>>>> Calling set_cpus_allowed_ptr():"
			" %s for %s\n", buf, p->comm);
#endif
	set_cpus_allowed_ptr(p, conn->conn_cpumask);
}

#else
#define iscsit_thread_get_cpumask(X) ({})
#define iscsit_thread_check_cpumask(X, Y, Z) ({})
#endif /* CONFIG_SMP */

int iscsi_target_tx_thread(void *arg)
{
	u8 state;
	int eodr = 0, map_sg = 0, ret = 0, sent_status = 0, use_misc = 0;
	struct iscsi_cmd *cmd = NULL;
	struct iscsi_conn *conn;
	struct iscsi_queue_req *qr = NULL;
	struct se_cmd *se_cmd;
	struct iscsi_thread_set *ts = (struct iscsi_thread_set *)arg;
	struct se_unmap_sg unmap_sg;
	/*
	 * Allow ourselves to be interrupted by SIGINT so that a
	 * connection recovery / failure event can be triggered externally.
	 */
	allow_signal(SIGINT);

restart:
	conn = iscsi_tx_thread_pre_handler(ts);
	if (!conn)
		goto out;

	eodr = map_sg = ret = sent_status = use_misc = 0;

	while (!kthread_should_stop()) {
		/*
		 * Ensure that both TX and RX per connection kthreads
		 * are scheduled to run on the same CPU.
		 */
		iscsit_thread_check_cpumask(conn, current, 1);

		schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT);

		if ((ts->status == ISCSI_THREAD_SET_RESET) ||
		     signal_pending(current))
			goto transport_err;

get_immediate:
		qr = iscsit_get_cmd_from_immediate_queue(conn);
		if (qr) {
			atomic_set(&conn->check_immediate_queue, 0);
			cmd = qr->cmd;
			state = qr->state;
			kmem_cache_free(lio_qr_cache, qr);

			spin_lock_bh(&cmd->istate_lock);
			switch (state) {
			case ISTATE_SEND_R2T:
				spin_unlock_bh(&cmd->istate_lock);
				ret = iscsit_send_r2t(cmd, conn);
				break;
			case ISTATE_REMOVE:
				spin_unlock_bh(&cmd->istate_lock);

				if (cmd->data_direction == DMA_TO_DEVICE)
					iscsit_stop_dataout_timer(cmd);

				spin_lock_bh(&conn->cmd_lock);
				list_del(&cmd->i_list);
				spin_unlock_bh(&conn->cmd_lock);
				/*
				 * Determine if a struct se_cmd is assoicated with
				 * this struct iscsi_cmd.
				 */
				if (!(SE_CMD(cmd)->se_cmd_flags & SCF_SE_LUN_CMD) &&
				    !(cmd->tmr_req))
					iscsit_release_cmd(cmd);
				else
					transport_generic_free_cmd(SE_CMD(cmd),
								1, 1, 0);
				goto get_immediate;
			case ISTATE_SEND_NOPIN_WANT_RESPONSE:
				spin_unlock_bh(&cmd->istate_lock);
				iscsit_mod_nopin_response_timer(conn);
				ret = iscsit_send_unsolicited_nopin(cmd,
						conn, 1);
				break;
			case ISTATE_SEND_NOPIN_NO_RESPONSE:
				spin_unlock_bh(&cmd->istate_lock);
				ret = iscsit_send_unsolicited_nopin(cmd,
						conn, 0);
				break;
			default:
				printk(KERN_ERR "Unknown Opcode: 0x%02x ITT:"
				" 0x%08x, i_state: %d on CID: %hu\n",
				cmd->iscsi_opcode, cmd->init_task_tag, state,
				conn->cid);
				spin_unlock_bh(&cmd->istate_lock);
				goto transport_err;
			}
			if (ret < 0) {
				conn->tx_immediate_queue = 0;
				goto transport_err;
			}

			if (iscsit_send_tx_data(cmd, conn, 1) < 0) {
				conn->tx_immediate_queue = 0;
				iscsit_tx_thread_wait_for_tcp(conn);
				goto transport_err;
			}

			spin_lock_bh(&cmd->istate_lock);
			switch (state) {
			case ISTATE_SEND_R2T:
				spin_unlock_bh(&cmd->istate_lock);
				spin_lock_bh(&cmd->dataout_timeout_lock);
				iscsit_start_dataout_timer(cmd, conn);
				spin_unlock_bh(&cmd->dataout_timeout_lock);
				break;
			case ISTATE_SEND_NOPIN_WANT_RESPONSE:
				cmd->i_state = ISTATE_SENT_NOPIN_WANT_RESPONSE;
				spin_unlock_bh(&cmd->istate_lock);
				break;
			case ISTATE_SEND_NOPIN_NO_RESPONSE:
				cmd->i_state = ISTATE_SENT_STATUS;
				spin_unlock_bh(&cmd->istate_lock);
				break;
			default:
				printk(KERN_ERR "Unknown Opcode: 0x%02x ITT:"
					" 0x%08x, i_state: %d on CID: %hu\n",
					cmd->iscsi_opcode, cmd->init_task_tag,
					state, conn->cid);
				spin_unlock_bh(&cmd->istate_lock);
				goto transport_err;
			}
			goto get_immediate;
		} else
			conn->tx_immediate_queue = 0;

get_response:
		qr = iscsit_get_cmd_from_response_queue(conn);
		if (qr) {
			cmd = qr->cmd;
			state = qr->state;
			kmem_cache_free(lio_qr_cache, qr);

			spin_lock_bh(&cmd->istate_lock);
check_rsp_state:
			switch (state) {
			case ISTATE_SEND_DATAIN:
				spin_unlock_bh(&cmd->istate_lock);
				memset(&unmap_sg, 0,
						sizeof(struct se_unmap_sg));
				unmap_sg.fabric_cmd = (void *)cmd;
				unmap_sg.se_cmd = SE_CMD(cmd);
				map_sg = 1;
				ret = iscsit_send_data_in(cmd, conn,
						&unmap_sg, &eodr);
				break;
			case ISTATE_SEND_STATUS:
			case ISTATE_SEND_STATUS_RECOVERY:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_status(cmd, conn);
				break;
			case ISTATE_SEND_LOGOUTRSP:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_logout_response(cmd, conn);
				break;
			case ISTATE_SEND_ASYNCMSG:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_conn_drop_async_message(
						cmd, conn);
				break;
			case ISTATE_SEND_NOPIN:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_nopin_response(cmd, conn);
				break;
			case ISTATE_SEND_REJECT:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_reject(cmd, conn);
				break;
			case ISTATE_SEND_TASKMGTRSP:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_task_mgt_rsp(cmd, conn);
				if (ret != 0)
					break;
				ret = iscsit_tmr_post_handler(cmd, conn);
				if (ret != 0)
					iscsit_fall_back_to_erl0(conn->sess);
				break;
			case ISTATE_SEND_TEXTRSP:
				spin_unlock_bh(&cmd->istate_lock);
				use_misc = 1;
				ret = iscsit_send_text_rsp(cmd, conn);
				break;
			default:
				printk(KERN_ERR "Unknown Opcode: 0x%02x ITT:"
					" 0x%08x, i_state: %d on CID: %hu\n",
					cmd->iscsi_opcode, cmd->init_task_tag,
					state, conn->cid);
				spin_unlock_bh(&cmd->istate_lock);
				goto transport_err;
			}
			if (ret < 0) {
				conn->tx_response_queue = 0;
				goto transport_err;
			}

			se_cmd = &cmd->se_cmd;

			if (map_sg && !conn->conn_ops->IFMarker &&
			    se_cmd->t_tasks_se_num) {
				iscsit_map_SG_segments(&unmap_sg);
				if (iscsit_fe_sendpage_sg(&unmap_sg, conn) < 0) {
					conn->tx_response_queue = 0;
					iscsit_tx_thread_wait_for_tcp(conn);
					iscsit_unmap_SG_segments(&unmap_sg);
					goto transport_err;
				}
				iscsit_unmap_SG_segments(&unmap_sg);
				map_sg = 0;
			} else {
				if (map_sg)
					iscsit_map_SG_segments(&unmap_sg);
				if (iscsit_send_tx_data(cmd, conn, use_misc) < 0) {
					conn->tx_response_queue = 0;
					iscsit_tx_thread_wait_for_tcp(conn);
					if (map_sg)
						iscsit_unmap_SG_segments(&unmap_sg);
					goto transport_err;
				}
				if (map_sg) {
					iscsit_unmap_SG_segments(&unmap_sg);
					map_sg = 0;
				}
			}

			spin_lock_bh(&cmd->istate_lock);
			switch (state) {
			case ISTATE_SEND_DATAIN:
				if (!eodr)
					goto check_rsp_state;

				if (eodr == 1) {
					cmd->i_state = ISTATE_SENT_LAST_DATAIN;
					sent_status = 1;
					eodr = use_misc = 0;
				} else if (eodr == 2) {
					cmd->i_state = state =
							ISTATE_SEND_STATUS;
					sent_status = 0;
					eodr = use_misc = 0;
					goto check_rsp_state;
				}
				break;
			case ISTATE_SEND_STATUS:
				use_misc = 0;
				sent_status = 1;
				break;
			case ISTATE_SEND_ASYNCMSG:
			case ISTATE_SEND_NOPIN:
			case ISTATE_SEND_STATUS_RECOVERY:
			case ISTATE_SEND_TEXTRSP:
				use_misc = 0;
				sent_status = 1;
				break;
			case ISTATE_SEND_REJECT:
				use_misc = 0;
				if (cmd->cmd_flags & ICF_REJECT_FAIL_CONN) {
					cmd->cmd_flags &= ~ICF_REJECT_FAIL_CONN;
					spin_unlock_bh(&cmd->istate_lock);
					complete(&cmd->reject_comp);
					goto transport_err;
				}
				complete(&cmd->reject_comp);
				break;
			case ISTATE_SEND_TASKMGTRSP:
				use_misc = 0;
				sent_status = 1;
				break;
			case ISTATE_SEND_LOGOUTRSP:
				spin_unlock_bh(&cmd->istate_lock);
				if (!(iscsit_logout_post_handler(cmd, conn)))
					goto restart;
				spin_lock_bh(&cmd->istate_lock);
				use_misc = 0;
				sent_status = 1;
				break;
			default:
				printk(KERN_ERR "Unknown Opcode: 0x%02x ITT:"
					" 0x%08x, i_state: %d on CID: %hu\n",
					cmd->iscsi_opcode, cmd->init_task_tag,
					cmd->i_state, conn->cid);
				spin_unlock_bh(&cmd->istate_lock);
				goto transport_err;
			}

			if (sent_status) {
				cmd->i_state = ISTATE_SENT_STATUS;
				sent_status = 0;
			}
			spin_unlock_bh(&cmd->istate_lock);

			if (atomic_read(&conn->check_immediate_queue))
				goto get_immediate;

			goto get_response;
		} else
			conn->tx_response_queue = 0;
	}

transport_err:
	iscsit_take_action_for_connection_exit(conn);
	goto restart;
out:
	return 0;
}

int iscsi_target_rx_thread(void *arg)
{
	int ret;
	u8 buffer[ISCSI_HDR_LEN], opcode;
	u32 checksum = 0, digest = 0;
	struct iscsi_conn *conn = NULL;
	struct iscsi_thread_set *ts = (struct iscsi_thread_set *)arg;
	struct kvec iov;
	/*
	 * Allow ourselves to be interrupted by SIGINT so that a
	 * connection recovery / failure event can be triggered externally.
	 */
	allow_signal(SIGINT);

restart:
	conn = iscsi_rx_thread_pre_handler(ts);
	if (!conn)
		goto out;

	while (!kthread_should_stop()) {
		/*
		 * Ensure that both TX and RX per connection kthreads
		 * are scheduled to run on the same CPU.
		 */
		iscsit_thread_check_cpumask(conn, current, 0);

		memset(buffer, 0, ISCSI_HDR_LEN);
		memset(&iov, 0, sizeof(struct kvec));

		iov.iov_base	= buffer;
		iov.iov_len	= ISCSI_HDR_LEN;

		ret = rx_data(conn, &iov, 1, ISCSI_HDR_LEN);
		if (ret != ISCSI_HDR_LEN) {
			iscsit_rx_thread_wait_for_tcp(conn);
			goto transport_err;
		}

		/*
		 * Set conn->bad_hdr for use with REJECT PDUs.
		 */
		memcpy(&conn->bad_hdr, &buffer, ISCSI_HDR_LEN);

		if (conn->conn_ops->HeaderDigest) {
			iov.iov_base	= &digest;
			iov.iov_len	= ISCSI_CRC_LEN;

			ret = rx_data(conn, &iov, 1, ISCSI_CRC_LEN);
			if (ret != ISCSI_CRC_LEN) {
				iscsit_rx_thread_wait_for_tcp(conn);
				goto transport_err;
			}

			iscsit_do_crypto_hash_buf(&conn->conn_rx_hash,
					buffer, ISCSI_HDR_LEN,
					0, NULL, (u8 *)&checksum);

			if (digest != checksum) {
				printk(KERN_ERR "HeaderDigest CRC32C failed,"
					" received 0x%08x, computed 0x%08x\n",
					digest, checksum);
				/*
				 * Set the PDU to 0xff so it will intentionally
				 * hit default in the switch below.
				 */
				memset(buffer, 0xff, ISCSI_HDR_LEN);
				spin_lock_bh(&conn->sess->session_stats_lock);
				conn->sess->conn_digest_errors++;
				spin_unlock_bh(&conn->sess->session_stats_lock);
			} else {
				TRACE(TRACE_DIGEST, "Got HeaderDigest CRC32C"
						" 0x%08x\n", checksum);
			}
		}

		if (conn->conn_state == TARG_CONN_STATE_IN_LOGOUT)
			goto transport_err;

		opcode = buffer[0] & ISCSI_OPCODE_MASK;

		if (conn->sess->sess_ops->SessionType &&
		   ((!(opcode & ISCSI_OP_TEXT)) ||
		    (!(opcode & ISCSI_OP_LOGOUT)))) {
			printk(KERN_ERR "Received illegal iSCSI Opcode: 0x%02x"
			" while in Discovery Session, rejecting.\n", opcode);
			iscsit_add_reject(ISCSI_REASON_PROTOCOL_ERROR, 1,
					buffer, conn);
			goto transport_err;
		}

		switch (opcode) {
		case ISCSI_OP_SCSI_CMD:
			if (iscsit_handle_scsi_cmd(conn, buffer) < 0)
				goto transport_err;
			break;
		case ISCSI_OP_SCSI_DATA_OUT:
			if (iscsit_handle_data_out(conn, buffer) < 0)
				goto transport_err;
			break;
		case ISCSI_OP_NOOP_OUT:
			if (iscsit_handle_nop_out(conn, buffer) < 0)
				goto transport_err;
			break;
		case ISCSI_OP_SCSI_TMFUNC:
			if (iscsit_handle_task_mgt_cmd(conn, buffer) < 0)
				goto transport_err;
			break;
		case ISCSI_OP_TEXT:
			if (iscsit_handle_text_cmd(conn, buffer) < 0)
				goto transport_err;
			break;
		case ISCSI_OP_LOGOUT:
			ret = iscsit_handle_logout_cmd(conn, buffer);
			if (ret > 0) {
				wait_for_completion_timeout(&conn->conn_logout_comp,
						SECONDS_FOR_LOGOUT_COMP * HZ);
				goto transport_err;
			} else if (ret < 0)
				goto transport_err;
			break;
		case ISCSI_OP_SNACK:
			if (iscsit_handle_snack(conn, buffer) < 0)
				goto transport_err;
			break;
		default:
			printk(KERN_ERR "Got unknown iSCSI OpCode: 0x%02x\n",
					opcode);
			if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
				printk(KERN_ERR "Cannot recover from unknown"
				" opcode while ERL=0, closing iSCSI connection"
				".\n");
				goto transport_err;
			}
			if (!conn->conn_ops->OFMarker) {
				printk(KERN_ERR "Unable to recover from unknown"
				" opcode while OFMarker=No, closing iSCSI"
					" connection.\n");
				goto transport_err;
			}
			if (iscsit_recover_from_unknown_opcode(conn) < 0) {
				printk(KERN_ERR "Unable to recover from unknown"
					" opcode, closing iSCSI connection.\n");
				goto transport_err;
			}
			break;
		}
	}

transport_err:
	if (!signal_pending(current))
		atomic_set(&conn->transport_failed, 1);
	iscsit_take_action_for_connection_exit(conn);
	goto restart;
out:
	return 0;
}

static void iscsit_release_commands_from_conn(struct iscsi_conn *conn)
{
	struct iscsi_cmd *cmd = NULL, *cmd_tmp = NULL;
	struct iscsi_session *sess = conn->sess;
	struct se_cmd *se_cmd;
	/*
	 * We expect this function to only ever be called from either RX or TX
	 * thread context via iscsit_close_connection() once the other context
	 * has been reset -> returned sleeping pre-handler state.
	 */
	spin_lock_bh(&conn->cmd_lock);
	list_for_each_entry_safe(cmd, cmd_tmp, &conn->conn_cmd_list, i_list) {
		if (!(SE_CMD(cmd)) ||
		    !(SE_CMD(cmd)->se_cmd_flags & SCF_SE_LUN_CMD)) {

			list_del(&cmd->i_list);
			spin_unlock_bh(&conn->cmd_lock);
			iscsit_increment_maxcmdsn(cmd, sess);
			se_cmd = SE_CMD(cmd);
			/*
			 * Special cases for active iSCSI TMR, and
			 * transport_lookup_cmd_lun() failing from
			 * iscsit_get_lun_for_cmd() in iscsit_handle_scsi_cmd().
			 */
			if (cmd->tmr_req && se_cmd->transport_wait_for_tasks)
				se_cmd->transport_wait_for_tasks(se_cmd, 1, 1);
			else if (SE_CMD(cmd)->se_cmd_flags & SCF_SE_LUN_CMD)
				transport_release_cmd_to_pool(se_cmd);
			else
				iscsit_release_cmd(cmd);

			spin_lock_bh(&conn->cmd_lock);
			continue;
		}
		list_del(&cmd->i_list);
		spin_unlock_bh(&conn->cmd_lock);

		iscsit_increment_maxcmdsn(cmd, sess);
		se_cmd = SE_CMD(cmd);

		if (se_cmd->transport_wait_for_tasks)
			se_cmd->transport_wait_for_tasks(se_cmd, 1, 1);

		spin_lock_bh(&conn->cmd_lock);
	}
	spin_unlock_bh(&conn->cmd_lock);
}

static void iscsit_stop_timers_for_cmds(
	struct iscsi_conn *conn)
{
	struct iscsi_cmd *cmd;

	spin_lock_bh(&conn->cmd_lock);
	list_for_each_entry(cmd, &conn->conn_cmd_list, i_list) {
		if (cmd->data_direction == DMA_TO_DEVICE)
			iscsit_stop_dataout_timer(cmd);
	}
	spin_unlock_bh(&conn->cmd_lock);
}

int iscsit_close_connection(
	struct iscsi_conn *conn)
{
	int conn_logout = (conn->conn_state == TARG_CONN_STATE_IN_LOGOUT);
	struct iscsi_session	*sess = conn->sess;

	TRACE(TRACE_ISCSI, "Closing iSCSI connection CID %hu on SID:"
		" %u\n", conn->cid, sess->sid);
	/*
	 * Always up conn_logout_comp just in case the RX Thread is sleeping
	 * and the logout response never got sent because the connection
	 * failed.
	 */
	complete(&conn->conn_logout_comp);

	iscsi_release_thread_set(conn);

	iscsit_stop_timers_for_cmds(conn);
	iscsit_stop_nopin_response_timer(conn);
	iscsit_stop_nopin_timer(conn);
	iscsit_free_queue_reqs_for_conn(conn);

	/*
	 * During Connection recovery drop unacknowledged out of order
	 * commands for this connection, and prepare the other commands
	 * for realligence.
	 *
	 * During normal operation clear the out of order commands (but
	 * do not free the struct iscsi_ooo_cmdsn's) and release all
	 * struct iscsi_cmds.
	 */
	if (atomic_read(&conn->connection_recovery)) {
		iscsit_discard_unacknowledged_ooo_cmdsns_for_conn(conn);
		iscsit_prepare_cmds_for_realligance(conn);
	} else {
		iscsit_clear_ooo_cmdsns_for_conn(conn);
		iscsit_release_commands_from_conn(conn);
	}

	/*
	 * Handle decrementing session or connection usage count if
	 * a logout response was not able to be sent because the
	 * connection failed.  Fall back to Session Recovery here.
	 */
	if (atomic_read(&conn->conn_logout_remove)) {
		if (conn->conn_logout_reason == ISCSI_LOGOUT_REASON_CLOSE_SESSION) {
			iscsit_dec_conn_usage_count(conn);
			iscsit_dec_session_usage_count(sess);
		}
		if (conn->conn_logout_reason == ISCSI_LOGOUT_REASON_CLOSE_CONNECTION)
			iscsit_dec_conn_usage_count(conn);

		atomic_set(&conn->conn_logout_remove, 0);
		atomic_set(&sess->session_reinstatement, 0);
		atomic_set(&sess->session_fall_back_to_erl0, 1);
	}

	spin_lock_bh(&sess->conn_lock);
	list_del(&conn->conn_list);

	/*
	 * Attempt to let the Initiator know this connection failed by
	 * sending an Connection Dropped Async Message on another
	 * active connection.
	 */
	if (atomic_read(&conn->connection_recovery))
		iscsit_build_conn_drop_async_message(conn);

	spin_unlock_bh(&sess->conn_lock);

	/*
	 * If connection reinstatement is being performed on this connection,
	 * up the connection reinstatement semaphore that is being blocked on
	 * in iscsit_cause_connection_reinstatement().
	 */
	spin_lock_bh(&conn->state_lock);
	if (atomic_read(&conn->sleep_on_conn_wait_comp)) {
		spin_unlock_bh(&conn->state_lock);
		complete(&conn->conn_wait_comp);
		wait_for_completion(&conn->conn_post_wait_comp);
		spin_lock_bh(&conn->state_lock);
	}

	/*
	 * If connection reinstatement is being performed on this connection
	 * by receiving a REMOVECONNFORRECOVERY logout request, up the
	 * connection wait rcfr semaphore that is being blocked on
	 * an iscsit_connection_reinstatement_rcfr().
	 */
	if (atomic_read(&conn->connection_wait_rcfr)) {
		spin_unlock_bh(&conn->state_lock);
		complete(&conn->conn_wait_rcfr_comp);
		wait_for_completion(&conn->conn_post_wait_comp);
		spin_lock_bh(&conn->state_lock);
	}
	atomic_set(&conn->connection_reinstatement, 1);
	spin_unlock_bh(&conn->state_lock);

	/*
	 * If any other processes are accessing this connection pointer we
	 * must wait until they have completed.
	 */
	iscsit_check_conn_usage_count(conn);

	if (conn->conn_rx_hash.tfm)
		crypto_free_hash(conn->conn_rx_hash.tfm);
	if (conn->conn_tx_hash.tfm)
		crypto_free_hash(conn->conn_tx_hash.tfm);

	if (conn->conn_cpumask)
		free_cpumask_var(conn->conn_cpumask);

	kfree(conn->conn_ops);
	conn->conn_ops = NULL;

	if (conn->sock) {
		if (conn->conn_flags & CONNFLAG_SCTP_STRUCT_FILE) {
			kfree(conn->sock->file);
			conn->sock->file = NULL;
		}
		sock_release(conn->sock);
	}
	conn->thread_set = NULL;

	TRACE(TRACE_STATE, "Moving to TARG_CONN_STATE_FREE.\n");
	conn->conn_state = TARG_CONN_STATE_FREE;
	kfree(conn);

	spin_lock_bh(&sess->conn_lock);
	atomic_dec(&sess->nconn);
	printk(KERN_INFO "Decremented iSCSI connection count to %hu from node:"
		" %s\n", atomic_read(&sess->nconn),
		sess->sess_ops->InitiatorName);
	/*
	 * Make sure that if one connection fails in an non ERL=2 iSCSI
	 * Session that they all fail.
	 */
	if ((sess->sess_ops->ErrorRecoveryLevel != 2) && !conn_logout &&
	     !atomic_read(&sess->session_logout))
		atomic_set(&sess->session_fall_back_to_erl0, 1);

	/*
	 * If this was not the last connection in the session, and we are
	 * performing session reinstatement or falling back to ERL=0, call
	 * iscsit_stop_session() without sleeping to shutdown the other
	 * active connections.
	 */
	if (atomic_read(&sess->nconn)) {
		if (!atomic_read(&sess->session_reinstatement) &&
		    !atomic_read(&sess->session_fall_back_to_erl0)) {
			spin_unlock_bh(&sess->conn_lock);
			return 0;
		}
		if (!atomic_read(&sess->session_stop_active)) {
			atomic_set(&sess->session_stop_active, 1);
			spin_unlock_bh(&sess->conn_lock);
			iscsit_stop_session(sess, 0, 0);
			return 0;
		}
		spin_unlock_bh(&sess->conn_lock);
		return 0;
	}

	/*
	 * If this was the last connection in the session and one of the
	 * following is occurring:
	 *
	 * Session Reinstatement is not being performed, and are falling back
	 * to ERL=0 call iscsit_close_session().
	 *
	 * Session Logout was requested.  iscsit_close_session() will be called
	 * elsewhere.
	 *
	 * Session Continuation is not being performed, start the Time2Retain
	 * handler and check if sleep_on_sess_wait_sem is active.
	 */
	if (!atomic_read(&sess->session_reinstatement) &&
	     atomic_read(&sess->session_fall_back_to_erl0)) {
		spin_unlock_bh(&sess->conn_lock);
		iscsit_close_session(sess);

		return 0;
	} else if (atomic_read(&sess->session_logout)) {
		TRACE(TRACE_STATE, "Moving to TARG_SESS_STATE_FREE.\n");
		sess->session_state = TARG_SESS_STATE_FREE;
		spin_unlock_bh(&sess->conn_lock);

		if (atomic_read(&sess->sleep_on_sess_wait_comp))
			complete(&sess->session_wait_comp);

		return 0;
	} else {
		TRACE(TRACE_STATE, "Moving to TARG_SESS_STATE_FAILED.\n");
		sess->session_state = TARG_SESS_STATE_FAILED;

		if (!atomic_read(&sess->session_continuation)) {
			spin_unlock_bh(&sess->conn_lock);
			iscsit_start_time2retain_handler(sess);
		} else
			spin_unlock_bh(&sess->conn_lock);

		if (atomic_read(&sess->sleep_on_sess_wait_comp))
			complete(&sess->session_wait_comp);

		return 0;
	}
	spin_unlock_bh(&sess->conn_lock);

	return 0;
}

int iscsit_close_session(struct iscsi_session *sess)
{
	struct iscsi_portal_group *tpg = ISCSI_TPG_S(sess);
	struct se_portal_group *se_tpg = &tpg->tpg_se_tpg;

	if (atomic_read(&sess->nconn)) {
		printk(KERN_ERR "%d connection(s) still exist for iSCSI session"
			" to %s\n", atomic_read(&sess->nconn),
			sess->sess_ops->InitiatorName);
		BUG();
	}

	spin_lock_bh(&se_tpg->session_lock);
	atomic_set(&sess->session_logout, 1);
	atomic_set(&sess->session_reinstatement, 1);
	iscsit_stop_time2retain_timer(sess);
	spin_unlock_bh(&se_tpg->session_lock);

	/*
	 * transport_deregister_session_configfs() will clear the
	 * struct se_node_acl->nacl_sess pointer now as a iscsi_np process context
	 * can be setting it again with __transport_register_session() in
	 * iscsi_post_login_handler() again after the iscsit_stop_session()
	 * completes in iscsi_np context.
	 */
	transport_deregister_session_configfs(sess->se_sess);

	/*
	 * If any other processes are accessing this session pointer we must
	 * wait until they have completed.  If we are in an interrupt (the
	 * time2retain handler) and contain and active session usage count we
	 * restart the timer and exit.
	 */
	if (!in_interrupt()) {
		if (iscsit_check_session_usage_count(sess) == 1)
			iscsit_stop_session(sess, 1, 1);
	} else {
		if (iscsit_check_session_usage_count(sess) == 2) {
			atomic_set(&sess->session_logout, 0);
			iscsit_start_time2retain_handler(sess);
			return 0;
		}
	}

	transport_deregister_session(sess->se_sess);

	if (sess->sess_ops->ErrorRecoveryLevel == 2)
		iscsit_free_connection_recovery_entires(sess);

	iscsit_free_all_ooo_cmdsns(sess);

	spin_lock_bh(&se_tpg->session_lock);
	TRACE(TRACE_STATE, "Moving to TARG_SESS_STATE_FREE.\n");
	sess->session_state = TARG_SESS_STATE_FREE;
	printk(KERN_INFO "Released iSCSI session from node: %s\n",
			sess->sess_ops->InitiatorName);
	tpg->nsessions--;
	if (tpg->tpg_tiqn)
		tpg->tpg_tiqn->tiqn_nsessions--;

	printk(KERN_INFO "Decremented number of active iSCSI Sessions on"
		" iSCSI TPG: %hu to %u\n", tpg->tpgt, tpg->nsessions);

	spin_lock(&sess_idr_lock);
	idr_remove(&sess_idr, sess->session_index);
	spin_unlock(&sess_idr_lock);

	kfree(sess->sess_ops);
	sess->sess_ops = NULL;
	spin_unlock_bh(&se_tpg->session_lock);

	kfree(sess);
	return 0;
}

static void iscsit_logout_post_handler_closesession(
	struct iscsi_conn *conn)
{
	struct iscsi_session *sess = conn->sess;

	iscsi_set_thread_clear(conn, ISCSI_CLEAR_TX_THREAD);
	iscsi_set_thread_set_signal(conn, ISCSI_SIGNAL_TX_THREAD);

	atomic_set(&conn->conn_logout_remove, 0);
	complete(&conn->conn_logout_comp);

	iscsit_dec_conn_usage_count(conn);
	iscsit_stop_session(sess, 1, 1);
	iscsit_dec_session_usage_count(sess);
	iscsit_close_session(sess);
}

static void iscsit_logout_post_handler_samecid(
	struct iscsi_conn *conn)
{
	iscsi_set_thread_clear(conn, ISCSI_CLEAR_TX_THREAD);
	iscsi_set_thread_set_signal(conn, ISCSI_SIGNAL_TX_THREAD);

	atomic_set(&conn->conn_logout_remove, 0);
	complete(&conn->conn_logout_comp);

	iscsit_cause_connection_reinstatement(conn, 1);
	iscsit_dec_conn_usage_count(conn);
}

static void iscsit_logout_post_handler_diffcid(
	struct iscsi_conn *conn,
	u16 cid)
{
	struct iscsi_conn *l_conn;
	struct iscsi_session *sess = conn->sess;

	if (!sess)
		return;

	spin_lock_bh(&sess->conn_lock);
	list_for_each_entry(l_conn, &sess->sess_conn_list, conn_list) {
		if (l_conn->cid == cid) {
			iscsit_inc_conn_usage_count(l_conn);
			break;
		}
	}
	spin_unlock_bh(&sess->conn_lock);

	if (!l_conn)
		return;

	if (l_conn->sock)
		l_conn->sock->ops->shutdown(l_conn->sock, RCV_SHUTDOWN);

	spin_lock_bh(&l_conn->state_lock);
	TRACE(TRACE_STATE, "Moving to TARG_CONN_STATE_IN_LOGOUT.\n");
	l_conn->conn_state = TARG_CONN_STATE_IN_LOGOUT;
	spin_unlock_bh(&l_conn->state_lock);

	iscsit_cause_connection_reinstatement(l_conn, 1);
	iscsit_dec_conn_usage_count(l_conn);
}

/*
 *	Return of 0 causes the TX thread to restart.
 */
static int iscsit_logout_post_handler(
	struct iscsi_cmd *cmd,
	struct iscsi_conn *conn)
{
	int ret = 0;

	switch (cmd->logout_reason) {
	case ISCSI_LOGOUT_REASON_CLOSE_SESSION:
		switch (cmd->logout_response) {
		case ISCSI_LOGOUT_SUCCESS:
		case ISCSI_LOGOUT_CLEANUP_FAILED:
		default:
			iscsit_logout_post_handler_closesession(conn);
			break;
		}
		ret = 0;
		break;
	case ISCSI_LOGOUT_REASON_CLOSE_CONNECTION:
		if (conn->cid == cmd->logout_cid) {
			switch (cmd->logout_response) {
			case ISCSI_LOGOUT_SUCCESS:
			case ISCSI_LOGOUT_CLEANUP_FAILED:
			default:
				iscsit_logout_post_handler_samecid(conn);
				break;
			}
			ret = 0;
		} else {
			switch (cmd->logout_response) {
			case ISCSI_LOGOUT_SUCCESS:
				iscsit_logout_post_handler_diffcid(conn,
					cmd->logout_cid);
				break;
			case ISCSI_LOGOUT_CID_NOT_FOUND:
			case ISCSI_LOGOUT_CLEANUP_FAILED:
			default:
				break;
			}
			ret = 1;
		}
		break;
	case ISCSI_LOGOUT_REASON_RECOVERY:
		switch (cmd->logout_response) {
		case ISCSI_LOGOUT_SUCCESS:
		case ISCSI_LOGOUT_CID_NOT_FOUND:
		case ISCSI_LOGOUT_RECOVERY_UNSUPPORTED:
		case ISCSI_LOGOUT_CLEANUP_FAILED:
		default:
			break;
		}
		ret = 1;
		break;
	default:
		break;

	}
	return ret;
}

void iscsit_fail_session(struct iscsi_session *sess)
{
	struct iscsi_conn *conn;

	spin_lock_bh(&sess->conn_lock);
	list_for_each_entry(conn, &sess->sess_conn_list, conn_list) {
		TRACE(TRACE_STATE, "Moving to TARG_CONN_STATE_CLEANUP_WAIT.\n");
		conn->conn_state = TARG_CONN_STATE_CLEANUP_WAIT;
	}
	spin_unlock_bh(&sess->conn_lock);

	TRACE(TRACE_STATE, "Moving to TARG_SESS_STATE_FAILED.\n");
	sess->session_state = TARG_SESS_STATE_FAILED;
}

int iscsit_free_session(struct iscsi_session *sess)
{
	u16 conn_count = atomic_read(&sess->nconn);
	struct iscsi_conn *conn, *conn_tmp = NULL;
	int is_last;

	spin_lock_bh(&sess->conn_lock);
	atomic_set(&sess->sleep_on_sess_wait_comp, 1);

	list_for_each_entry_safe(conn, conn_tmp, &sess->sess_conn_list,
			conn_list) {
		if (conn_count == 0)
			break;

		if (list_is_last(&conn->conn_list, &sess->sess_conn_list)) {
			is_last = 1;
		} else {
			iscsit_inc_conn_usage_count(conn_tmp);
			is_last = 0;
		}
		iscsit_inc_conn_usage_count(conn);

		spin_unlock_bh(&sess->conn_lock);
		iscsit_cause_connection_reinstatement(conn, 1);
		spin_lock_bh(&sess->conn_lock);

		iscsit_dec_conn_usage_count(conn);
		if (is_last == 0)
			iscsit_dec_conn_usage_count(conn_tmp);
	
		conn_count--;
	}

	if (atomic_read(&sess->nconn)) {
		spin_unlock_bh(&sess->conn_lock);
		wait_for_completion(&sess->session_wait_comp);
	} else
		spin_unlock_bh(&sess->conn_lock);

	iscsit_close_session(sess);
	return 0;
}

void iscsit_stop_session(
	struct iscsi_session *sess,
	int session_sleep,
	int connection_sleep)
{
	u16 conn_count = atomic_read(&sess->nconn);
	struct iscsi_conn *conn, *conn_tmp = NULL;
	int is_last;

	spin_lock_bh(&sess->conn_lock);
	if (session_sleep)
		atomic_set(&sess->sleep_on_sess_wait_comp, 1);

	if (connection_sleep) {
		list_for_each_entry_safe(conn, conn_tmp, &sess->sess_conn_list,
				conn_list) {
			if (conn_count == 0)
				break;

			if (list_is_last(&conn->conn_list, &sess->sess_conn_list)) {
				is_last = 1;
			} else {
				iscsit_inc_conn_usage_count(conn_tmp);
				is_last = 0;
			}
			iscsit_inc_conn_usage_count(conn);

			spin_unlock_bh(&sess->conn_lock);
			iscsit_cause_connection_reinstatement(conn, 1);
			spin_lock_bh(&sess->conn_lock);

			iscsit_dec_conn_usage_count(conn);
			if (is_last == 0)
				iscsit_dec_conn_usage_count(conn_tmp);
			conn_count--;
		}
	} else {
		list_for_each_entry(conn, &sess->sess_conn_list, conn_list)
			iscsit_cause_connection_reinstatement(conn, 0);
	}

	if (session_sleep && atomic_read(&sess->nconn)) {
		spin_unlock_bh(&sess->conn_lock);
		wait_for_completion(&sess->session_wait_comp);
	} else
		spin_unlock_bh(&sess->conn_lock);
}

int iscsit_release_sessions_for_tpg(struct iscsi_portal_group *tpg, int force)
{
	struct iscsi_session *sess;
	struct se_portal_group *se_tpg = &tpg->tpg_se_tpg;
	struct se_session *se_sess, *se_sess_tmp;
	int session_count = 0;

	spin_lock_bh(&se_tpg->session_lock);
	if (tpg->nsessions && !force) {
		spin_unlock_bh(&se_tpg->session_lock);
		return -1;
	}

	list_for_each_entry_safe(se_sess, se_sess_tmp, &se_tpg->tpg_sess_list,
			sess_list) {
		sess = (struct iscsi_session *)se_sess->fabric_sess_ptr;

		spin_lock(&sess->conn_lock);
		if (atomic_read(&sess->session_fall_back_to_erl0) ||
		    atomic_read(&sess->session_logout) ||
		    (sess->time2retain_timer_flags & ISCSI_TF_EXPIRED)) {
			spin_unlock(&sess->conn_lock);
			continue;
		}
		atomic_set(&sess->session_reinstatement, 1);
		spin_unlock(&sess->conn_lock);
		spin_unlock_bh(&se_tpg->session_lock);

		iscsit_free_session(sess);
		spin_lock_bh(&se_tpg->session_lock);

		session_count++;
	}
	spin_unlock_bh(&se_tpg->session_lock);

	TRACE(TRACE_ISCSI, "Released %d iSCSI Session(s) from Target Portal"
			" Group: %hu\n", session_count, tpg->tpgt);
	return 0;
}

MODULE_DESCRIPTION("iSCSI-Target Driver for mainline target infrastructure");
MODULE_VERSION("4.1.x");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

module_init(iscsi_target_init_module);
module_exit(iscsi_target_cleanup_module);