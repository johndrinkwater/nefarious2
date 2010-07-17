/** @file geoip.h
 * @brief GeoIP routine definitions.
 * @version $Id$
 */
#ifndef INCLUDED_geoip_h
#define INCLUDED_geoip_h

#include "client.h"
#include "GeoIP.h"

extern void geoip_init(void);
extern void geoip_apply(struct Client* cptr);
extern void geoip_handle_enable(void);
extern void geoip_handle_file(void);
extern void geoip_handle_ipv6_file(void);

#endif /* INCLUDED_geoip_h */

