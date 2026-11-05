#ifndef IMS_ROUTES_H
#define IMS_ROUTES_H

int parse_route_failure(const char *line, char *host, char *gateway);
void delete_stale_ims_routes(void);
void add_missing_ims_routes(void);

#endif /* IMS_ROUTES_H */
