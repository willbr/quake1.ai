/*
cl_dlight_colors.h -- named per-event dynamic-light colours.

These are vec3_t multipliers applied to each channel in
R_AddDynamicLights_RGB. {1,1,1} is white (the historical mono behaviour).
Tune values here without touching call sites.
*/

#ifndef CL_DLIGHT_COLORS_H
#define CL_DLIGHT_COLORS_H

static const vec3_t DLIGHT_COLOR_WHITE      = {1.00f, 1.00f, 1.00f};
static const vec3_t DLIGHT_COLOR_MUZZLE     = {1.00f, 0.85f, 0.45f};  // warm yellow
static const vec3_t DLIGHT_COLOR_ROCKET     = {1.00f, 0.60f, 0.20f};  // orange
static const vec3_t DLIGHT_COLOR_EXPLOSION  = {1.00f, 0.50f, 0.20f};  // orange-red
static const vec3_t DLIGHT_COLOR_LIGHTNING  = {0.60f, 0.70f, 1.00f};  // pale blue
static const vec3_t DLIGHT_COLOR_BRIGHTLIGHT = {1.00f, 0.90f, 0.70f}; // warm
static const vec3_t DLIGHT_COLOR_DIMLIGHT   = {1.00f, 0.85f, 0.60f};  // warm

#endif
