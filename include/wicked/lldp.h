/*
 * LLDP agent support (transmit-only) for wicked
 *
 * Copyright (C) 2013 Olaf Kirch <okir@suse.de>
 */

#ifndef __WICKED_LLDP_H__
#define __WICKED_LLDP_H__

#include <wicked/types.h>
#include <wicked/constants.h>
#include <wicked/address.h>

/* Chassis ID subtype */
typedef enum ni_lldp_chassis_id_type {
	NI_LLDP_CHASSIS_ID_INVALID		= 0,
	NI_LLDP_CHASSIS_ID_CHASSIS_COMPONENT	= 1,
	NI_LLDP_CHASSIS_ID_INTERFACE_ALIAS	= 2,
	NI_LLDP_CHASSIS_ID_PORT_COMPONENT	= 3,
	NI_LLDP_CHASSIS_ID_MAC_ADDRESS		= 4,
	NI_LLDP_CHASSIS_ID_NETWORK_ADDRESS	= 5,
	NI_LLDP_CHASSIS_ID_INTERFACE_NAME	= 6,
	NI_LLDP_CHASSIS_ID_LOCALLY_ASSIGNED	= 7,
} ni_lldp_chassis_id_type_t;

/* Port ID subtype */
typedef enum ni_lldp_port_id_type {
	NI_LLDP_PORT_ID_INVALID			= 0,
	NI_LLDP_PORT_ID_INTERFACE_ALIAS		= 1,
	NI_LLDP_PORT_ID_PORT_COMPONENT		= 2,
	NI_LLDP_PORT_ID_MAC_ADDRESS		= 3,
	NI_LLDP_PORT_ID_NETWORK_ADDRESS		= 4,
	NI_LLDP_PORT_ID_INTERFACE_NAME		= 5,
	NI_LLDP_PORT_ID_AGENT_CIRCUIT_ID	= 6,
	NI_LLDP_PORT_ID_LOCALLY_ASSIGNED	= 7,
} ni_lldp_port_id_type_t;

struct ni_lldp {
	uint32_t				destination;

	struct {
		ni_lldp_chassis_id_type_t	type;
		char *				string_value;
		ni_hwaddr_t			mac_addr_value;
		ni_sockaddr_t			net_addr_value;
	} chassis_id;

	struct {
		ni_lldp_port_id_type_t		type;
		char *				string_value;
		ni_hwaddr_t			mac_addr_value;
		ni_sockaddr_t			net_addr_value;
	} port_id;

	uint32_t				ttl;
};


extern ni_lldp_t *	ni_lldp_new(void);
extern void		ni_lldp_free(ni_lldp_t *);
extern int		ni_system_lldp_setup(ni_netconfig_t *nc, ni_netdev_t *, const ni_lldp_t *);

extern const char *	ni_lldp_destination_type_to_name(ni_lldp_destination_t);

#endif /* __WICKED_LLDP_H__ */
