/*
 * Generic dbus client functions for address configuration
 * services implemented as separate DBus services (like dhcp,
 * ipv4ll)
 *
 * Copyright (C) 2011 Olaf Kirch <okir@suse.de>
 */

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/addrconf.h>
#include <wicked/system.h>
#include "netinfo_priv.h"	/* for __ni_system_interface_update_lease */
#include "dbus-common.h"
#include "model.h"
#include "debug.h"


#define WICKED_DBUS_ADDRCONF_IPV4STATIC_INTERFACE	WICKED_DBUS_INTERFACE ".Addrconf.ipv4.static"
#define WICKED_DBUS_ADDRCONF_IPV6STATIC_INTERFACE	WICKED_DBUS_INTERFACE ".Addrconf.ipv6.static"

/*
 * Interface.acquire(dict options)
 * Acquire a lease for the given interface.
 *
 * The options dictionary contains addrconf request properties.
 */
int
ni_objectmodel_addrconf_acquire(ni_dbus_object_t *object, const ni_addrconf_request_t *req)
{
	DBusError error = DBUS_ERROR_INIT;
	ni_dbus_variant_t argument;
	int rv = 0;

	if (req == NULL)
		return -NI_ERROR_INVALID_ARGS;

	ni_dbus_variant_init_dict(&argument);
	if (!__wicked_dbus_get_addrconf_request(req, &argument, &error))
		goto translate_error;

	if (!ni_dbus_object_call_variant(object, NULL, "acquire", 1, &argument, 0, NULL, &error))
		goto translate_error;

	rv = TRUE;

failed:
	ni_dbus_variant_destroy(&argument);
	dbus_error_free(&error);
	return rv;

translate_error:
	rv = ni_dbus_object_translate_error(object, &error);
	goto failed;
}

/*
 * Interface.release()
 * Release a lease for the given interface.
 *
 * The options dictionary contains addrconf request properties.
 */
int
ni_objectmodel_addrconf_release(ni_dbus_object_t *object, const ni_addrconf_lease_t *lease)
{
	DBusError error = DBUS_ERROR_INIT;
	ni_dbus_variant_t argv[1];
	int argc = 0;
	int rv = 0;

	if (lease != NULL) {
		ni_dbus_variant_set_uuid(&argv[argc], &lease->uuid);
		argc++;
	}

	if (!ni_dbus_object_call_variant(object, NULL, "drop", argc, argv, 0, NULL, &error))
		rv = ni_dbus_object_translate_error(object, &error);

	while (argc--)
		ni_dbus_variant_destroy(&argv[0]);
	dbus_error_free(&error);
	return rv;
}

/*
 * Extract interface index from object path.
 * Path names must be WICKED_DBUS_OBJECT_PATH "/" <something> "/Interface/" <index>
 */
static ni_interface_t *
ni_objectmodel_addrconf_path_to_device(const char *path)
{
	unsigned int ifindex;
	ni_netconfig_t *nc;
	char cc;

	if (strncmp(path, WICKED_DBUS_OBJECT_PATH, strlen(WICKED_DBUS_OBJECT_PATH)))
		return NULL;
	path += strlen(WICKED_DBUS_OBJECT_PATH);

	if (*path++ != '/')
		return NULL;
	while ((cc = *path++) != '/') {
		if (cc == '\0')
			return NULL;
	}

	if (strncmp(path, "Interface/", 10))
		return NULL;
	path += 10;

	if (ni_parse_int(path, &ifindex) < 0)
		return NULL;

	nc = ni_global_state_handle(1);
	if (nc == NULL) {
		ni_error("%s: unable to refresh interfaces", __func__);
		return NULL;
	}

	return ni_interface_by_index(nc, ifindex);
}

static ni_addrconf_lease_t *
ni_objectmodel_interface_to_lease(const char *interface)
{
	if (!strcmp(interface, WICKED_DBUS_DHCP4_INTERFACE))
		return ni_addrconf_lease_new(NI_ADDRCONF_DHCP, AF_INET);

	return NULL;
}

/*
 * Callback from addrconf supplicant whenever it acquired, released or lost a lease.
 *
 * FIXME SECURITY:
 * Is it good enough to check for the sender interface to avoid that someone is sending
 * us spoofed lease messages?!
 */
void
ni_objectmodel_addrconf_signal_handler(ni_dbus_connection_t *conn, ni_dbus_message_t *msg, void *user_data)
{
	const char *signal_name = dbus_message_get_member(msg);
	ni_interface_t *ifp;
	ni_addrconf_lease_t *lease = NULL;
	ni_dbus_variant_t argv[16];
	int argc;

	memset(argv, 0, sizeof(argv));
	argc = ni_dbus_message_get_args_variants(msg, argv, 16);
	if (argc < 0) {
		ni_error("%s: cannot parse arguments for signal %s", __func__, signal_name);
		goto done;
	}

	ifp = ni_objectmodel_addrconf_path_to_device(dbus_message_get_path(msg));
	if (ifp == NULL) {
		ni_debug_dbus("%s: received signal %s for unknown interface %s", __func__,
				signal_name, dbus_message_get_path(msg));
		goto done;
	}

	lease = ni_objectmodel_interface_to_lease(dbus_message_get_interface(msg));
	if (lease == NULL) {
		ni_debug_dbus("received signal %s from %s (unknown service)",
				signal_name, dbus_message_get_interface(msg));
		goto done;
	}

	if (argc >= 1 && !ni_objectmodel_set_addrconf_lease(lease, &argv[0])) {
		ni_debug_dbus("%s: unable to parse lease argument", __func__);
		goto done;
	}

	ni_debug_dbus("received signal %s for interface %s (ifindex %d), lease %s/%s",
			signal_name, ifp->name, ifp->link.ifindex,
			ni_addrconf_type_to_name(lease->type),
			ni_addrfamily_type_to_name(lease->family));
	if (!strcmp(signal_name, "LeaseAcquired")) {
		if (lease->state != NI_ADDRCONF_STATE_GRANTED) {
			ni_error("%s: unexpected lease state in signal %s", __func__, signal_name);
			goto done;
		}

		/* Note, lease may be NULL after this, as the interface object
		 * takes ownership of it. */
		__ni_system_interface_update_lease(ifp, &lease);

		if (__ni_interface_is_up(ifp))
			ni_objectmodel_interface_event(NULL, ifp, NI_EVENT_NETWORK_UP);
	} else if (!strcmp(signal_name, "LeaseReleased")) {
		lease->state = NI_ADDRCONF_STATE_RELEASED;
		__ni_system_interface_update_lease(ifp, &lease);

		if (__ni_interface_is_down(ifp))
			ni_objectmodel_interface_event(NULL, ifp, NI_EVENT_NETWORK_DOWN);
	} else if (!strcmp(signal_name, "LeaseLost")) {
		lease->state = NI_ADDRCONF_STATE_FAILED;
		__ni_system_interface_update_lease(ifp, &lease);
		ni_objectmodel_interface_event(NULL, ifp, NI_EVENT_ADDRESS_LOST);
	} else {
		/* Ignore unknown signal */
	}

done:
	while (argc--)
		ni_dbus_variant_destroy(&argv[argc]);
	if (lease)
		ni_addrconf_lease_free(lease);
}

/*
 * Verbatim copy from interface.c
 */
static ni_interface_t *
get_interface(const ni_dbus_object_t *object, DBusError *error)
{
	ni_interface_t *dev;

	if (!(dev = ni_objectmodel_unwrap_interface(object))) {
		dbus_set_error(error,
				DBUS_ERROR_FAILED,
				"Method not compatible with object %s (not a network interface)",
				object->path);
		return NULL;
	}
	return dev;
}

/*
 * Configure static IPv4 addresses
 */
static dbus_bool_t
ni_objectmodel_addrconf_ipv4_static_configure(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_addrconf_request_t *req = NULL;
	const ni_dbus_variant_t *dict;
	ni_interface_t *dev;
	int rv;

	if (!(dev = get_interface(object, error)))
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_is_dict(&argv[0])) {
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
				"%s.%s: exected one dict argument",
				WICKED_DBUS_ADDRCONF_IPV4STATIC_INTERFACE, method->name);
		return FALSE;
	}
	dict = &argv[0];

	req = ni_addrconf_request_new(NI_ADDRCONF_STATIC, AF_INET);
	if (!__ni_objectmodel_set_address_dict(&req->statik.addrs, dict, error)
	 || !__ni_objectmodel_set_route_dict(&req->statik.routes, dict, error))
		return FALSE;

	rv = ni_system_interface_addrconf(ni_global_state_handle(0), dev, req);
	ni_addrconf_request_free(req);

	if (rv < 0) {
		dbus_set_error(error,
				DBUS_ERROR_FAILED,
				"Error configuring static IPv4 addresses: %s",
				ni_strerror(rv));
		return FALSE;
	} else {
		/* A NULL event ID tells the caller that we're done, there's no event
		 * to wait for. */
		ni_dbus_message_append_uint32(reply, 0);
	}

	return TRUE;
}

/*
 * Configure static IPv6 addresses
 */
static dbus_bool_t
ni_objectmodel_addrconf_ipv6_static_configure(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_addrconf_request_t *req = NULL;
	const ni_dbus_variant_t *dict;
	ni_interface_t *dev;
	int rv;

	if (!(dev = get_interface(object, error)))
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_is_dict(&argv[0])) {
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
				"%s.%s: exected one dict argument",
				WICKED_DBUS_ADDRCONF_IPV4STATIC_INTERFACE, method->name);
		return FALSE;
	}
	dict = &argv[0];

	req = ni_addrconf_request_new(NI_ADDRCONF_STATIC, AF_INET6);
	if (!__ni_objectmodel_set_address_dict(&req->statik.addrs, dict, error)
	 || !__ni_objectmodel_set_route_dict(&req->statik.routes, dict, error))
		return FALSE;

	rv = ni_system_interface_addrconf(ni_global_state_handle(0), dev, req);
	ni_addrconf_request_free(req);

	if (rv < 0) {
		dbus_set_error(error,
				DBUS_ERROR_FAILED,
				"Error configuring static IPv6 addresses: %s",
				ni_strerror(rv));
		return FALSE;
	} else {
		/* A NULL event ID tells the caller that we're done, there's no event
		 * to wait for. */
		ni_dbus_message_append_uint32(reply, 0);
	}

	return TRUE;
}

#if 0
/*
 * Get/set properties of a static addrconf request
 */
static dbus_bool_t
__ni_objectmodel_addrconf_get_address(const ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				DBusError *error)
{
	return FALSE;
}

static dbus_bool_t
__ni_objectmodel_addrconf_set_address(ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				const ni_dbus_variant_t *argument,
				DBusError *error)
{
	return FALSE;
}
#endif

dbus_bool_t
__ni_objectmodel_addrconfreq_get_address_properties(const ni_addrconf_request_t *req,
				ni_dbus_variant_t *dict,
				DBusError *error)
{
	if (!__ni_objectmodel_get_address_dict(req->statik.addrs, dict, error))
		return FALSE;

	return TRUE;
}

/*
 * Addrconf methods
 */
static const ni_dbus_method_t		ni_objectmodel_addrconf_ipv4_static_methods[] = {
	{ "configure",		"a{sv}",		ni_objectmodel_addrconf_ipv4_static_configure },
	{ NULL }
};

static const ni_dbus_method_t		ni_objectmodel_addrconf_ipv6_static_methods[] = {
	{ "configure",		"a{sv}",		ni_objectmodel_addrconf_ipv6_static_configure },
	{ NULL }
};

/*
 * IPv4 and IPv6 addrconf requests share the same properties
 */
#define ADDRCONF_PROPERTY(type, __name, rw) \
	NI_DBUS_PROPERTY(type, __name, __ni_objectmodel_addrconf, rw)
#define ADDRCONF_PROPERTY_SIGNATURE(signature, __name, rw) \
	__NI_DBUS_PROPERTY(signature, __name,  __ni_objectmodel_addrconf, rw)

static ni_dbus_property_t		ni_objectmodel_addrconf_static_properties[] = {
//	ADDRCONF_PROPERTY_SIGNATURE(NI_DBUS_DICT_SIGNATURE, address, RO),
	{ NULL }
};

ni_dbus_service_t			ni_objectmodel_addrconf_ipv4_static_service = {
	.name		= WICKED_DBUS_ADDRCONF_IPV4STATIC_INTERFACE,
	/* The .compatible member is filled in through dbus-xml. Not nice. */
//	.compatible	= &ni_objectmodel_netif_class,
	.properties	= ni_objectmodel_addrconf_static_properties,
	.methods	= ni_objectmodel_addrconf_ipv4_static_methods,
};

ni_dbus_service_t			ni_objectmodel_addrconf_ipv6_static_service = {
	.name		= WICKED_DBUS_ADDRCONF_IPV4STATIC_INTERFACE,
	/* The .compatible member is filled in through dbus-xml. Not nice. */
//	.compatible	= &ni_objectmodel_netif_class,
	.properties	= ni_objectmodel_addrconf_static_properties,
	.methods	= ni_objectmodel_addrconf_ipv6_static_methods,
};
