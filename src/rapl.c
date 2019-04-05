/*
Copyright (c) 2012, Intel Corporation
Copyright (c) 2015-2018, Dirk Beyer

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions
and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or other materials provided with
the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse
or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * For reference, see
 * http://software.intel.com/en-us/articles/power-gov
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cpuid.h"
#include "intel-family.h"
#include "msr.h"
#include "rapl.h"
#include "rapl-impl.h"
#include "util.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

uint64_t debug_enabled = 0;

char *RAPL_DOMAIN_STRINGS[RAPL_NR_DOMAIN] = {"package", "core", "uncore", "dram", "psys"};
char *RAPL_DOMAIN_FORMATTED_STRINGS[RAPL_NR_DOMAIN] = {"Package", "Core", "Uncore", "DRAM", "PSYS"};

/* rapl msr availablility */
#define MSR_SUPPORT_MASK 0xff
unsigned char *msr_support_table;

/* Global Variables */
double RAPL_TIME_UNIT;
double RAPL_ENERGY_UNIT;
double RAPL_DRAM_ENERGY_UNIT;
double RAPL_POWER_UNIT;

uint32_t processor_signature;

uint64_t num_nodes = 0;

static node_t *pkg_map; // node-to-cpu mapping

typedef struct rapl_unit_multiplier_t {
  double power;
  double energy;
  double time;
} rapl_unit_multiplier_t;

// For documentation, see:
// http://software.intel.com/en-us/articles/intel-64-architecture-processor-topology-enumeration
int build_topology() {

  int err;
  uint64_t max_pkg = 0;
  const uint64_t os_cpu_count = sysconf(_SC_NPROCESSORS_CONF);

  // Construct an os map: os_map[APIC_ID ... APIC_ID]
  APIC_ID_t os_map[os_cpu_count];

  for (uint64_t i = 0; i < os_cpu_count; i++) {

    err = get_core_information(i, &(os_map[i]));

    if (os_map[i].pkg_id > max_pkg) {
      max_pkg = os_map[i].pkg_id;
    }

    // printf("smt_id: %u core_id: %u pkg_id: %u os_id: %u\n",
    //   os_map[i].smt_id, os_map[i].core_id, os_map[i].pkg_id, i);
  }

  num_nodes = max_pkg + 1;

  // Construct a pkg map: pkg_map[pkg id] = (os_id of first thread on pkg)
  pkg_map = (node_t *)malloc(num_nodes * sizeof(node_t));

  for (uint64_t i = 0; i < os_cpu_count; i++) {
    uint64_t p = os_map[i].pkg_id;
    assert(p < num_nodes);
    if (os_map[i].smt_id == 0 && os_map[i].core_id == 0) {
      pkg_map[p] = i;
    }
  }

  return err;
}

void config_msr_table() {
  // calloc sets the allocated memory to zero (unlike malloc, where this is not the case)
  msr_support_table = (unsigned char *)calloc(MSR_SUPPORT_MASK, sizeof(unsigned char));

  uint64_t msr = 0;
  int cpu = 0;
  int err_read_msr = 0;

  // values for unit-multiplier
  err_read_msr = read_msr(cpu, MSR_RAPL_POWER_UNIT, &msr);
  msr_support_table[MSR_RAPL_POWER_UNIT & MSR_SUPPORT_MASK] = !err_read_msr;

  // values for package-msr
  err_read_msr = read_msr(cpu, MSR_RAPL_PKG_POWER_LIMIT, &msr);
  msr_support_table[MSR_RAPL_PKG_POWER_LIMIT & MSR_SUPPORT_MASK] = !err_read_msr;
  err_read_msr = read_msr(cpu, MSR_RAPL_PKG_ENERGY_STATUS, &msr);
  msr_support_table[MSR_RAPL_PKG_ENERGY_STATUS & MSR_SUPPORT_MASK] = !err_read_msr;
  err_read_msr = read_msr(cpu, MSR_RAPL_PKG_POWER_INFO, &msr);
  msr_support_table[MSR_RAPL_PKG_POWER_INFO & MSR_SUPPORT_MASK] = !err_read_msr;

  // values for dram msr
  err_read_msr = read_msr(cpu, MSR_RAPL_DRAM_POWER_LIMIT, &msr);
  msr_support_table[MSR_RAPL_DRAM_POWER_LIMIT & MSR_SUPPORT_MASK] = !err_read_msr;
  err_read_msr = read_msr(cpu, MSR_RAPL_DRAM_ENERGY_STATUS, &msr);
  msr_support_table[MSR_RAPL_DRAM_ENERGY_STATUS & MSR_SUPPORT_MASK] = !err_read_msr;

  // values for core msr
  err_read_msr = read_msr(cpu, MSR_RAPL_PP0_POWER_LIMIT, &msr);
  msr_support_table[MSR_RAPL_PP0_POWER_LIMIT & MSR_SUPPORT_MASK] = !err_read_msr;
  err_read_msr = read_msr(cpu, MSR_RAPL_PP0_ENERGY_STATUS, &msr);
  msr_support_table[MSR_RAPL_PP0_ENERGY_STATUS & MSR_SUPPORT_MASK] = !err_read_msr;

  // values for uncore msr
  err_read_msr = read_msr(cpu, MSR_RAPL_PP1_POWER_LIMIT, &msr);
  msr_support_table[MSR_RAPL_PP1_POWER_LIMIT & MSR_SUPPORT_MASK] = !err_read_msr;
  err_read_msr = read_msr(cpu, MSR_RAPL_PP1_ENERGY_STATUS, &msr);
  msr_support_table[MSR_RAPL_PP1_ENERGY_STATUS & MSR_SUPPORT_MASK] = !err_read_msr;

  // value for psys msr
  err_read_msr = read_msr(cpu, MSR_RAPL_PLATFORM_ENERGY_STATUS, &msr);
  msr_support_table[MSR_RAPL_PLATFORM_ENERGY_STATUS & MSR_SUPPORT_MASK] = !err_read_msr;
}

node_t get_cpu_from_node(node_t node) {
#ifndef TEST
  return pkg_map[node];
#else // simply return value 0 when unit-testing, as the pkg_map isn't initialized then
  return 0;
#endif
}

/*!
 * This function must be called before calling any other function from this class.
 * \return 0 on success, -1 otherwise
 */
int init_rapl() {
  int err = 0;

  char vendor[VENDOR_LENGTH];
  get_vendor_name(vendor);
  if (!is_intel_processor()) {
#ifndef TEST // don't print the error-msg when unit-testing
    fprintf(stderr,
            "The processor on the working machine is not from Intel. Found %s-processor instead.\n",
            vendor);
#endif
    return MY_ERROR;
  }
  if (debug_enabled) {
    fprintf(stdout, "[DEBUG] %s processor found.\n", vendor);
  }

  unsigned int family;
  processor_signature = get_processor_signature();
  family = (processor_signature >> 8) & 0xf;
  if (debug_enabled) {
    fprintf(stdout, "[DEBUG] Processor is from family %d and uses model 0x%05X.\n", family,
            (processor_signature & 0xfffffff0));
  }
  if (family != 6) {
  // CPUID.family == 6 means it's anything from Pentium Pro (1995) to the latest Kaby Lake (2017)
  // except "Netburst"
#ifndef TEST // don't print the error-msg when unit-testing
    fprintf(
        stderr,
        "The Intel processor must be from family 6, but instead a cpu from family %d was found.\n",
        family);
#endif
    return MY_ERROR;
  }

  err = build_topology();
  if (err) {
    fprintf(stderr, "An error occured while binding the cpu and/or the context.\n");
    return MY_ERROR;
  }

  err = open_msr_fd(num_nodes, &get_cpu_from_node);
  if (err) {
    fprintf(stderr, "An error occurred while opening the msr file through a FD.\n");
    return MY_ERROR;
  }

  config_msr_table();

  err = read_rapl_units();

  /* 32 is the width of these fields when they are stored */
  MAX_ENERGY_STATUS_JOULES = (double)(RAPL_ENERGY_UNIT * (pow(2, 32) - 1));

  return err;
}

/*!
 * Call this function function to cleanup resources.
 * \return 0 on success
 */
int terminate_rapl() {
  close_msr_fd();

  if (NULL != pkg_map) {
    free(pkg_map);
  }

  if (NULL != msr_support_table) {
    free(msr_support_table);
  }

  return 0;
}

/*!
 * \brief Check if MSR is supported on this machine.
 * \return 1 if supported, 0 otherwise
 */
uint64_t is_supported_msr(uint64_t msr) {
  return (uint64_t)msr_support_table[msr & MSR_SUPPORT_MASK];
}

/*!
 * \brief Check if power domain (PKG, PP0, PP1, DRAM) is supported on this machine.
 *
 * Currently server parts support: PKG, PP0 and DRAM and client parts support PKG, PP0 and PP1.
 *
 * \return 1 if supported, 0 otherwise
 */
uint64_t is_supported_domain(uint64_t power_domain) {
  switch (power_domain) {
  case RAPL_PKG:
    return is_supported_msr(MSR_RAPL_PKG_ENERGY_STATUS);
  case RAPL_PP0:
    return is_supported_msr(MSR_RAPL_PP0_ENERGY_STATUS);
  case RAPL_PP1:
    return is_supported_msr(MSR_RAPL_PP1_ENERGY_STATUS);
  case RAPL_DRAM:
    return is_supported_msr(MSR_RAPL_DRAM_ENERGY_STATUS);
  case RAPL_PSYS:
    return is_supported_msr(MSR_RAPL_PLATFORM_ENERGY_STATUS);
  default:
    abort();
  }
}

/*!
 * \brief Get the number of RAPL nodes on this machine.
 *
 * Get the number of package power domains, that you can control using RAPL. This is equal to the
 * number of CPU packages in the system.
 *
 * \return number of RAPL nodes.
 */
uint64_t get_num_rapl_nodes() {
  return num_nodes;
}

int get_rapl_unit_multiplier(uint64_t node, rapl_unit_multiplier_t *unit_obj) {
  int err = 0;
  uint64_t msr = 0;
  rapl_unit_multiplier_msr_t unit_msr;

  err = !is_supported_msr(MSR_RAPL_POWER_UNIT);
  if (!err) {
    err = read_msr(node, MSR_RAPL_POWER_UNIT, &msr);
  }
  if (!err) {
    unit_msr = *(rapl_unit_multiplier_msr_t *)&msr;

    unit_obj->time = 1.0 / (double)(B2POW(unit_msr.time));
    unit_obj->energy = 1.0 / (double)(B2POW(unit_msr.energy));
    unit_obj->power = 1.0 / (double)(B2POW(unit_msr.power));
  }

  return err;
}

/* Common methods (should not be interfaced directly) */

int get_total_energy_consumed(uint64_t node, uint64_t msr_address,
                              double *total_energy_consumed_joules) {
  int err = 0;
  uint64_t msr = 0;
  energy_status_msr_t domain_msr;
  cpu_set_t old_context;

  err = !is_supported_msr(msr_address);
  if (!err) {
    bind_cpu(get_cpu_from_node(node), &old_context); // improve performance on Linux
    err = read_msr(node, msr_address, &msr);
    bind_context(&old_context, NULL);
  }

  if (!err) {
    domain_msr = *(energy_status_msr_t *)&msr;

    switch (msr_address) {
    case MSR_RAPL_DRAM_ENERGY_STATUS:
      *total_energy_consumed_joules = RAPL_DRAM_ENERGY_UNIT * domain_msr.total_energy_consumed;
      break;
    default:
      *total_energy_consumed_joules = RAPL_ENERGY_UNIT * domain_msr.total_energy_consumed;
      break;
    }
  }

  return err;
}

/*!
 * \brief Get a pointer to the RAPL PKG energy consumed register.
 *
 * This read-only register provides energy consumed in joules for the package power domain since the
 * last machine reboot (or energy register wraparound)
 *
 * \return 0 on success, -1 otherwise
 */
int get_pkg_total_energy_consumed(uint64_t node, double *total_energy_consumed_joules) {
  return get_total_energy_consumed(node, MSR_RAPL_PKG_ENERGY_STATUS, total_energy_consumed_joules);
}

/*
 * Energy units are either hard-coded, or come from RAPL Energy Unit MSR.
 */
double rapl_dram_energy_units_probe(double rapl_energy_units) {
  switch (processor_signature & 0xfffffff0) {
  case CPU_INTEL_HASWELL_X:
  case CPU_INTEL_BROADWELL_X:
  case CPU_INTEL_BROADWELL_XEON_D:
  case CPU_INTEL_SKYLAKE_X:
  case CPU_INTEL_XEON_PHI_KNL:
  case CPU_INTEL_XEON_PHI_KNM:
    if (debug_enabled) {
      fprintf(stdout, "[DEBUG] Using a predefined unit for measuring the rapl DRAM values: %.6e\n",
              15.3E-6);
    }
    return 15.3E-6;
  default:
    if (debug_enabled) {
      fprintf(stdout, "[DEBUG] Using the default unit for measuring the rapl DRAM values: %.6e\n",
              rapl_energy_units);
    }
    return rapl_energy_units;
  }
}

/*!
 * \brief Get a pointer to the RAPL DRAM energy consumed register.
 *
 * This read-only register provides energy consumed in joules for the DRAM power domain since the
 * last machine reboot (or energy register wraparound)
 *
 * \return 0 on success, -1 otherwise
 */
int get_dram_total_energy_consumed(uint64_t node, double *total_energy_consumed_joules) {
  return get_total_energy_consumed(node, MSR_RAPL_DRAM_ENERGY_STATUS, total_energy_consumed_joules);
}

/*!
 * \brief Get a pointer to the RAPL PP0 energy consumed register.
 *
 * This read-only register provides energy consumed in joules for the PP0 (core) power domain since
 * the last machine reboot (or energy register wraparound)
 *
 * \return 0 on success, -1 otherwise
 */
int get_pp0_total_energy_consumed(uint64_t node, double *total_energy_consumed_joules) {
  return get_total_energy_consumed(node, MSR_RAPL_PP0_ENERGY_STATUS, total_energy_consumed_joules);
}

/*!
 * \brief Get a pointer to the RAPL PP1 energy consumed register.
 *
 * This read-only register provides energy consumed in joules for the PP1 (uncore) power domain
 * since the last machine reboot (or energy register wraparound)
 *
 * \return 0 on success, -1 otherwise
 */
int get_pp1_total_energy_consumed(uint64_t node, double *total_energy_consumed_joules) {
  return get_total_energy_consumed(node, MSR_RAPL_PP1_ENERGY_STATUS, total_energy_consumed_joules);
}

/*!
 * \brief Get a pointer to the RAPL plattform energy (psys) consumed register.
 *
 * This read-only register provides energy consumed in joules for the PSYS domain since the last
 * machine reboot (or energy register wraparound)
 *
 * According to Intel Software Devlopers Manual Volume 4, Table 2-38, the consumed energy is the
 * total energy consumed by all devices in the plattform that receive power from integrated power
 * delivery mechanism. Included plattform devices are processor cores, SOC, memory, add-on or
 * peripheral devies that get powered directly from the platform power delivery means.
 *
 * \return 0 on success, -1 otherwise
 */
int get_psys_total_energy_consumed(uint64_t node, double *total_energy_consumed_joules) {
  return get_total_energy_consumed(node, MSR_RAPL_PLATFORM_ENERGY_STATUS,
                                   total_energy_consumed_joules);
}

double convert_to_watts(unsigned int raw) {
  return RAPL_POWER_UNIT * raw;
}

double convert_to_seconds(unsigned int raw) {
  return RAPL_TIME_UNIT * raw;
}

/*!
 * \brief Get a pointer to the RAPL PKG power info register
 *
 * This read-only register provides information about the max/min power limiting settings available
 * on the machine. This register is defined in the pkg_rapl_parameters_t data structure.
 *
 * \return 0 on success, -1 otherwise
 */
int get_pkg_rapl_parameters(unsigned int node, pkg_rapl_parameters_t *pkg_obj) {
  int err = 0;
  uint64_t msr = 0;
  rapl_parameters_msr_t domain_msr;

  err = !is_supported_msr(MSR_RAPL_PKG_POWER_INFO);
  if (!err) {
    err = read_msr(node, MSR_RAPL_PKG_POWER_INFO, &msr);
  }

  if (!err) {
    domain_msr = *(rapl_parameters_msr_t *)&msr;

    pkg_obj->thermal_spec_power_watts = convert_to_watts(domain_msr.thermal_spec_power);
    pkg_obj->minimum_power_watts = convert_to_watts(domain_msr.minimum_power);
    pkg_obj->maximum_power_watts = convert_to_watts(domain_msr.maximum_power);
    pkg_obj->maximum_limit_time_window_seconds =
        convert_to_seconds(domain_msr.maximum_limit_time_window);
  }

  return err;
}

/*!
 * Calculates the measurement interval between the msr probes.
 * The goal is to measure as rarely as possible, but often enough so that no overflow will be
 * missed out.
 *
 * The calculation is based on the values of 'RAPL_ENERGY_UNIT' in [J] and 'thermal_spec_power' in
 * [W], and is computed as follows (unit in [s]econds):
 * ((2^32 - 1) * RAPL_ENERGY_UNIT) / thermal_spec_power
 *
 * For the final result, the value from the above formula is taken and divided by two afterwards.
 */
void calculate_probe_interval_time(struct timespec *signal_timelimit, double thermal_spec_power) {
  double result = ((pow(2, 32) - 1) * RAPL_ENERGY_UNIT) / thermal_spec_power;
  result = result / 2 - 1;

  long seconds = floor(result);
  long nano_seconds = result * pow(10, 9) - seconds * pow(10, 9);

  signal_timelimit->tv_sec = seconds;
  signal_timelimit->tv_nsec = nano_seconds;
}

/* Utilities */

int read_rapl_units() {
  int err = 0;
  rapl_unit_multiplier_t unit_multiplier;

  err = get_rapl_unit_multiplier(0, &unit_multiplier);
  if (!err) {
    RAPL_TIME_UNIT = unit_multiplier.time;
    RAPL_ENERGY_UNIT = unit_multiplier.energy;
    RAPL_DRAM_ENERGY_UNIT = rapl_dram_energy_units_probe(unit_multiplier.energy);
    RAPL_POWER_UNIT = unit_multiplier.power;
  }
  if (debug_enabled) {
    fprintf(stdout,
            "[DEBUG] Measured the following unit multipliers:\n"
            "[DEBUG] RAPL_ENERGY_UNIT: %0.6e J\n"
            "[DEBUG] RAPL_DRAM_ENERGY_UNIT: %0.6e J\n",
            RAPL_ENERGY_UNIT, RAPL_DRAM_ENERGY_UNIT);
  }

  return err;
}
