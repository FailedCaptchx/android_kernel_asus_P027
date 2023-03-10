#ifndef PD_DBG_INFO_H_INCLUDED
#define PD_DBG_INFO_H_INCLUDED

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb/tcpci_config.h>

#ifdef CONFIG_PD_DBG_INFO
extern int pd_dbg_info(const char *fmt, ...);
extern void pd_dbg_info_lock(void);
extern void pd_dbg_info_unlock(void);
#else
static inline int pd_dbg_info(const char *fmt, ...)
{
	return 0;
}
static inline void pd_dbg_info_lock(void) {}
static inline void pd_dbg_info_unlock(void) {}
#endif	/* CONFIG_PD_DBG_INFO */

#endif /* PD_DBG_INFO_H_INCLUDED */
