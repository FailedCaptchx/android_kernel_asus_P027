/*
 *  include/linux/usb/tcpci_typec.h
 *  Include header file for Richtek TCPC Interface TypeC Driver
 *
 *  Copyright (C) 2015 Richtek Technology Corp.
 *  Jeff Chang <jeff_chang@richtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef __LINUX_TCPCI_TYPEC_H
#define __LINUX_TCPCI_TYPEC_H
#include <linux/usb/tcpci.h>

struct tcpc_device;

/******************************************************************************
 *  Call following function to trigger TYPEC Connection State Change
 *
 * 1. H/W -> CC/PS Change.
 * 2. Timer -> CCDebounce or PDDebounce or others Timeout
 * 3. Policy Engine -> PR_SWAP, Error_Recovery, PE_Idle
 *****************************************************************************/

extern int tcpc_typec_handle_cc_change(
	struct tcpc_device *tcpc_dev);

extern int tcpc_typec_handle_ps_change(
		struct tcpc_device *tcpc_dev, int vbus_level);

extern int tcpc_typec_handle_timeout(
		struct tcpc_device *tcpc_dev, uint32_t timer_id);

#ifdef CONFIG_TCPC_VSAFE0V_DETECT
extern int tcpc_typec_handle_vsafe0v(struct tcpc_device *tcpc_dev);
#endif

extern int tcpc_typec_set_rp_level(struct tcpc_device *tcpc_dev, uint8_t res);

#ifdef CONFIG_USB_POWER_DELIVERY
extern int tcpc_typec_handle_pe_pr_swap(struct tcpc_device *tcpc_dev);
#else
extern int tcpc_typec_swap_role(struct tcpc_device *tcpc_dev);
#endif

#endif /* #ifndef __LINUX_TCPCI_TYPEC_H */
