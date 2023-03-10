#ifndef PUBLIC_INTERFACE_H
#define PUBLIC_INTERFACE_H
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include "anx_ohio_private_interface.h"
#include "anx_ohio_public_interface.h"

/**
 * @desc: The Interface that AP sends the specific USB PD command to Ohio
 *
 * @param:
 *	type: PD message type, define enum PD_MSG_TYPE.
 *	buf: the sepecific parameter pointer according to the message type
 *		eg: when AP update its source capability type=TYPE_PWR_SRC_CAP
 *		"buf" contains the content of PDO object,its format USB PD spec
 *		customer can easily packeted it through PDO_FIXED_XXX macro:
 *		default5Vsafe 5V,0.9A -> PDO_FIXED(5000,900, PDO_FIXED_FLAGS)
 *	size: the parameter content length. if buf is null, it should be 0
 *
 * @return:  0: success, Error: 1, reject 2: fail, 3: busy
 */
u8 send_pd_msg(PD_MSG_TYPE type, const char *buf, u8 size)
{
	u8 rst = 0;
	u8 wait_cmd_response_time = 0;

	switch (type) {
	case TYPE_PWR_SRC_CAP:
		rst = send_src_cap(buf, size);
		break;
	case TYPE_PWR_SNK_CAP:
		rst = send_snk_cap(buf, size);
		break;
	case TYPE_DP_SNK_IDENTITY:
		rst = interface_send_msg_timeout(TYPE_DP_SNK_IDENTITY,
						 (u8 *) buf, size, INTERFACE_TIMEOUT);
		break;
	case TYPE_SVID:
		rst = send_svid(buf, size);
		break;
	case TYPE_GET_DP_SNK_CAP:
		rst = interface_send_msg_timeout(TYPE_GET_DP_SNK_CAP, NULL, 0, INTERFACE_TIMEOUT);
		break;
	case TYPE_PSWAP_REQ:
		rst = send_power_swap();
		wait_cmd_response_time = 200;
		break;
	case TYPE_DSWAP_REQ:
		rst = send_data_swap();
		wait_cmd_response_time = 200;
		break;
	case TYPE_GOTO_MIN_REQ:
		rst = interface_send_gotomin();
		break;
	case TYPE_VDM:
		rst = send_vdm(buf, size);
		break;
	case TYPE_DP_SNK_CFG:
		rst = send_dp_snk_cfg(buf, size);
		break;
	case TYPE_PD_STATUS_REQ:
		rst = interface_get_pd_status();
		wait_cmd_response_time = 200;
		break;
	case TYPE_PWR_OBJ_REQ:
		rst = send_rdo(buf, size);
		break;
	case TYPE_ACCEPT:
		rst = interface_send_accept();
		break;
	case TYPE_REJECT:
		rst = interface_send_reject();
		break;
	case TYPE_SOFT_RST:
		rst = interface_send_soft_rst();
		wait_cmd_response_time = 0;
		break;
	case TYPE_HARD_RST:
		rst = interface_send_hard_rst();
		break;
	default:
		pr_info("unknown type %x\n", type);
		rst = 0;
		break;
	}
	if (rst == CMD_FAIL) {
		pr_err("Cmd %x Fail.\n", type);
		return CMD_FAIL;
	}
	/* need wait command's response */
	if (wait_cmd_response_time)
		rst = wait_pd_cmd_timeout((u8) type, wait_cmd_response_time);
	else
		rst = CMD_SUCCESS;

	return rst;
}

/**
 * @desc:   The Interface that AP handle the specific USB PD command from Ohio
 *
 * @param:
 *	type: PD message type, define enum PD_MSG_TYPE.
 *	para: the sepecific parameter pointer
 *	para_len: the parameter ponter's content length
 *		if buf is null, it should be 0
 *
 * @return:  0: success 1: fail
 *
 */
u8 dispatch_rcvd_pd_msg(PD_MSG_TYPE type, void *para, u8 para_len)
{
	u8 rst = 0;
	pd_callback_t fnc = get_pd_callback_fnc(type);

	if (fnc != 0) {
		rst = (*fnc) (para, para_len);
		return rst;
	}

	switch (type) {
	case TYPE_PWR_SRC_CAP:
		/* execute the receved source capability's  handle function */
		rst = recv_pd_source_caps_default_callback(para, para_len);
		break;
	case TYPE_PWR_SNK_CAP:
		/* received peer's sink caps */
		rst = recv_pd_sink_caps_default_callback(para, para_len);
		break;
	case TYPE_PWR_OBJ_REQ:
		/* evaluate RDO and give accpet or reject */
		rst = recv_pd_pwr_object_req_default_callback(para, para_len);
		break;
	case TYPE_DSWAP_REQ:
		/* execute the receved handle function */
		rst = recv_pd_dswap_default_callback(para, para_len);
		break;
	case TYPE_PSWAP_REQ:
		/* execute the receved handle function */
		rst = recv_pd_pswap_default_callback(para, para_len);
		break;

	case TYPE_VDM:
#ifdef SUPP_VDM_CHARGING
		if (((u8 *) para)[1] == 0x01 && ((u8 *) para)[0] == 0x00) {
			((u8 *) para)[0] = 0x40;
			interface_send_vdm_data(para, 2 * 4);
			pr_info("0 usVDM response\n");
		} else if (((u8 *) para)[1] == 0x01 && ((u8 *) para)[0] == 0x01) {
			((u8 *) para)[0] = 0x41;
			((u8 *) para)[4] = 0x1;
			((u8 *) para)[5] = 0x2;
			((u8 *) para)[6] = 0x3;
			((u8 *) para)[7] = 0x4;
			interface_send_vdm_data(para, 2 * 4);
			pr_info("1 usVDM response\n");
		}
		break;
#endif
	case TYPE_ACCEPT:
		rst = recv_pd_accept_default_callback(para, para_len);
		break;
	case TYPE_RESPONSE_TO_REQ:
		/* execute the receved handle function */
		rst = recv_pd_cmd_rsp_default_callback(para, para_len);
		break;

	case TYPE_DP_ALT_ENTER:
		pr_notice("DP_ALT Enter!\n");
		break;
	case TYPE_DP_ALT_EXIT:
		pr_notice("DP_ALT Exit!\n");
		break;
	case TYPE_HARD_RST:
		rst = recv_pd_hard_rst_default_callback(para, para_len);
		break;
	default:
		rst = 0;
		break;
	}
	return rst;
}

/**
 * @desc:  The Interface helps customer to register one's
 *	interesting callback function of the specific
 *	USB PD message type, when the REGISTERED message
 *	arrive, the customer's callback function will be executed.
 * !!!! Becarefully:
 *  Because the USB PD TIMING limatation, the callback function
 *  should be designed to follow USB PD timing requiment.
 *
 * @param:
 *	type: PD message type, define enum PD_MSG_TYPE.
 *	func: callback function pointer
 *		it's sepecific definaction is:u8 (*)(void *, u8)
 *
 * @return:  1: success 0: fail
 *
 */
u8 register_pd_msg_callback_func(PD_MSG_TYPE type, pd_callback_t fnc)
{
	if (type > 256)
		return 0;
	set_pd_callback_fnc(type, fnc);

	return 1;
}

#endif
