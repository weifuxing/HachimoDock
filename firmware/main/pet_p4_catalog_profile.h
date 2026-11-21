/* Two withdrawn component IDs may still own slots on upgraded devices.
 * Keep their data without taking any of the public installation capacity. */
#pragma once
#include "pet_p4_component_features.h"
#ifndef PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE
#define PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE 0
#endif
#if PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE && !PET_COMPONENT_PROGRAM_PARENT_UNIFIED
#error "Private acceptance capacity requires the complete unified parent"
#endif
#if PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE
#define PET_P4_MINIAPP_VISIBLE_MAX 41
#else
#define PET_P4_MINIAPP_VISIBLE_MAX 40
#endif
#define PET_P4_MINIAPP_CATALOG_MAX (PET_P4_MINIAPP_VISIBLE_MAX + 2)
