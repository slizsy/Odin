#ifndef ODIN_ISO3166_H
#define ODIN_ISO3166_H

// iso 3166-1 alpha-2 (case-insensitive) -> short english country name, or NULL if unknown
const char *iso3166_name(const char *alpha2);

#endif
