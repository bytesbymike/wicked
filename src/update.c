/*
 * Dynamically update system configuration from addrconf lease data
 *
 * Copyright (C) 2009-2010 Olaf Kirch <okir@suse.de>
 */

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/logging.h>
#include "netinfo_priv.h"
#include "config.h"

typedef int		ni_update_handler_t(ni_handle_t *, const ni_addrconf_lease_t *);

typedef struct ni_update_info {
	struct {
		unsigned int	ifindex;
		unsigned int	lease_type;
		unsigned int	lease_family;
	} origin;
} ni_update_info_t;

struct ni_update_lease_choice {
	ni_interface_t *	interface;
	ni_addrconf_lease_t *	lease;
};

static ni_update_info_t	__ni_update_info[__NI_ADDRCONF_UPDATE_MAX];
static ni_update_handler_t *__ni_update_handlers[__NI_ADDRCONF_UPDATE_MAX];

static void		ni_system_update_find_lease(ni_handle_t *, unsigned int, struct ni_update_lease_choice *);

/*
 * Determine our capabilities to update anything at all
 */
unsigned int
ni_system_update_capabilities(void)
{
	static unsigned int capabilities = 0;

	if (capabilities == 0) {
		unsigned int target;

		__ni_addrconf_set_update(&capabilities, NI_ADDRCONF_UPDATE_DEFAULT_ROUTE);
		for (target = 0; target < __NI_ADDRCONF_UPDATE_MAX; ++target) {
			if (__ni_update_handlers[target])
				__ni_addrconf_set_update(&capabilities, target);
		}
	}

	return capabilities;
}

/*
 * Determine a lease's capability and permissions to update anything
 * This is the intersection of what the lease was configured to update,
 * and what information was provided by the address configuration service.
 */
static unsigned int
ni_system_lease_capabilities(ni_interface_t *ifp, const ni_addrconf_lease_t *lease)
{
	ni_afinfo_t *afi = __ni_interface_address_info(ifp, lease->family);
	unsigned int mask = 0;

	if (ni_addrconf_lease_is_valid(lease)) {
		if (lease->hostname != NULL)
			__ni_addrconf_set_update(&mask, NI_ADDRCONF_UPDATE_HOSTNAME);
		if (lease->nis != NULL)
			__ni_addrconf_set_update(&mask, NI_ADDRCONF_UPDATE_NIS);
		if (lease->resolver != NULL)
			__ni_addrconf_set_update(&mask, NI_ADDRCONF_UPDATE_RESOLVER);

		if (afi->request[lease->type])
			mask &= afi->request[lease->type]->update;
	}

	return mask;
}

/*
 * Update a single service (NIS, resolver, hostname, ...) given the information from
 * the lease. When we get here, all policy decisions have been made, and we just
 * need to commit the information.
 */
static int
ni_system_update_service(ni_handle_t *nih, ni_interface_t *ifp, const ni_addrconf_lease_t *lease, unsigned int target)
{
	ni_update_info_t *info = &__ni_update_info[target];
	ni_update_handler_t *handler;

	if ((handler = __ni_update_handlers[target]) == NULL)
		return 0;

	ni_debug_ifconfig("trying to configure %s from %s/%s lease (device %s)",
				ni_addrconf_update_target_to_name(target),
				ni_addrconf_type_to_name(lease->type),
				ni_addrfamily_type_to_name(lease->family),
				ifp->name);

	if (handler(nih, lease) < 0) {
		ni_error("%s: failed to update %s information from %s/%s lease",
				ifp->name,
				ni_addrconf_update_target_to_name(target),
				ni_addrconf_type_to_name(lease->type),
				ni_addrfamily_type_to_name(lease->family));
		return -1;
	}

	info->origin.ifindex = ifp->ifindex;
	info->origin.lease_type = lease->type;
	info->origin.lease_family = lease->family;
	return 0;
}

/*
 * Restore a service's configuration to the original (system) default
 */
static void
ni_system_restore_service(ni_handle_t *nih, unsigned int target)
{
	ni_update_handler_t *handler;

	if ((handler = __ni_update_handlers[target]) != NULL)
		handler(nih, NULL);
}

/*
 * Update the system configuration given the information from an addrconf lease,
 * such as a DHCP lease.
 */
int
ni_system_update_from_lease(ni_handle_t *nih, ni_interface_t *ifp, const ni_addrconf_lease_t *lease)
{
	unsigned int update_permitted, update_mask = 0, clear_mask = 0;
	unsigned int target;
	int rv = 0;

	update_permitted = ni_config_addrconf_update_mask(ni_global.config, lease->type);
	update_permitted &= ni_system_update_capabilities();

	if (update_permitted == 0)
		return 0;

	update_mask = ni_system_lease_capabilities(ifp, lease);

	for (target = 0; target < __NI_ADDRCONF_UPDATE_MAX; ++target) {
		ni_update_info_t *info = &__ni_update_info[target];

		if (!__ni_addrconf_should_update(update_permitted, target))
			continue;

		/* If the specific config object is already configured by some
		 * lease, do not overwrite it unless it's the same service on the
		 * same interface.
		 * Note, we could also assign per-interface and per-lease-type
		 * weights to config information. Things would get complex though :-)
		 */
		if (info->origin.ifindex) {
			if (info->origin.ifindex != ifp->ifindex
			 || info->origin.lease_type != lease->type
			 || info->origin.lease_family != lease->family)
				continue;

			if (!__ni_addrconf_should_update(update_mask, target)) {
				/* We previously configured this with data from a
				 * lease, but the new lease does not have this
				 * information any more.
				 * This usually happens when the lease is dropped, and
				 * we get a lease in state RELEASED. However, this
				 * can also happen eg when we suspend a laptop,
				 * and wake it up on a completely different network. In
				 * that case, we may get a lease that has some but not
				 * all of the config items as the previous one.
				 */
				__ni_addrconf_set_update(&clear_mask, target);
				memset(&info->origin, 0, sizeof(info->origin));
				continue;
			}
		} else if (!__ni_addrconf_should_update(update_mask, target))
			continue;

		if (ni_system_update_service(nih, ifp, lease, target) < 0) {
			__ni_addrconf_set_update(&clear_mask, target);
			rv = -1;
		}
	}

	/*
	 * If we cleared some config items, try to fill them with the
	 * information from a different lease.
	 */
	update_mask = clear_mask;
	for (target = 0; target < __NI_ADDRCONF_UPDATE_MAX; ++target) {
		struct ni_update_lease_choice best = { NULL, NULL };

		if (!__ni_addrconf_should_update(update_mask, target))
			continue;

		ni_system_update_find_lease(nih, target, &best);
		if (best.lease == 0
		 || ni_system_update_service(nih, best.interface, best.lease, target) < 0) {
			/* Unable to configure the service. Deconfigure it completely,
			 * and restore the previously saved backup copy. */
			ni_system_restore_service(nih, target);
		}
	}

	/* FIXME: we need to run updater scripts. If we had gone through the
	 * REST interface, that code would have taken care of this. However,
	 * we went to the service functions directly, so we need to trigger the
	 * updater scripts here, manually. */

	return rv;
}

static void
ni_system_update_find_lease_afinfo(ni_interface_t *ifp, ni_afinfo_t *afi, unsigned int target, struct ni_update_lease_choice *best)
{
	unsigned int mode;

	for (mode = 0; mode < __NI_ADDRCONF_MAX; ++mode) {
		ni_addrconf_lease_t *lease = afi->lease[mode];
		unsigned int update_mask;

		update_mask = ni_system_lease_capabilities(ifp, lease);
		if (__ni_addrconf_should_update(update_mask, target)) {
			/* If we have several leases providing the required information,
			 * pick the oldest one. */
			if (best->lease == 0 || lease->time_acquired < best->lease->time_acquired) {
				best->interface = ifp;
				best->lease = lease;
			}
		}
	}
}

static void
ni_system_update_find_lease_interface(ni_interface_t *ifp, unsigned int target, struct ni_update_lease_choice *best)
{
	ni_system_update_find_lease_afinfo(ifp, &ifp->ipv4, target, best);
	ni_system_update_find_lease_afinfo(ifp, &ifp->ipv6, target, best);
}

static void
ni_system_update_find_lease(ni_handle_t *nih, unsigned int target, struct ni_update_lease_choice *best)
{
	ni_interface_t *ifp;

	/* Loop over all interfaces and check if we have another
	 * valid lease that would work here. */
	for (ifp = ni_interfaces(nih); ifp; ifp = ifp->next)
		ni_system_update_find_lease_interface(ifp, target, best);
}

/*
 * Functions for updating system configuration
 */
static int
__ni_update_hostname(ni_handle_t *nih, const ni_addrconf_lease_t *lease)
{
	if (nih->op->hostname_put == NULL) {
		ni_error("%s: operation not supported", __FUNCTION__);
		return -1;
	}

	if (lease == NULL)
		return 0;

	if (lease->hostname == NULL) {
		ni_error("%s: no hostname present", __FUNCTION__);
		return -1;
	}

	return nih->op->hostname_put(nih, lease->hostname);
}

static int
__ni_update_resolver(ni_handle_t *nih, const ni_addrconf_lease_t *lease)
{
	if (nih->op->resolver_put == NULL) {
		ni_error("%s: operation not supported", __FUNCTION__);
		return -1;
	}

	if (lease == NULL) {
		if (nih->op->resolver_restore)
			return nih->op->resolver_restore(nih);
		return 0;
	}

	if (lease->resolver == NULL) {
		ni_error("%s: no resolver config present", __FUNCTION__);
		return -1;
	}

	if (nih->op->resolver_backup && nih->op->resolver_backup(nih) < 0) {
		ni_error("%s: unable to back up original configuration", __FUNCTION__);
		return -1;
	}

	return nih->op->resolver_put(nih, lease->resolver);
}

static int
__ni_update_nis(ni_handle_t *nih, const ni_addrconf_lease_t *lease)
{
	if (nih->op->nis_put == NULL) {
		ni_error("%s: operation not supported", __FUNCTION__);
		return -1;
	}

	if (lease == NULL) {
		if (nih->op->nis_restore)
			return nih->op->nis_restore(nih);
		return 0;
	}

	if (lease->nis == NULL) {
		ni_error("%s: no nis config present", __FUNCTION__);
		return -1;
	}

	if (nih->op->nis_backup && nih->op->nis_backup(nih) < 0) {
		ni_error("%s: unable to back up original configuration", __FUNCTION__);
		return -1;
	}

	return nih->op->nis_put(nih, lease->nis);
}

static ni_update_handler_t *	__ni_update_handlers[__NI_ADDRCONF_UPDATE_MAX] = {
[NI_ADDRCONF_UPDATE_HOSTNAME]	= __ni_update_hostname,
[NI_ADDRCONF_UPDATE_RESOLVER]	= __ni_update_resolver,
[NI_ADDRCONF_UPDATE_NIS]	= __ni_update_nis,
};
