
//
//    Copyright (C) 2007-2008 Sebastian Kuzminsky
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//

#include <linux/slab.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "hal/drivers/mesa-hostmot2/hostmot2.h"




// this is the function exported to HAL
// it keeps the watchdog from biting us for a while
static void hm2_pet_watchdog(void *void_hm2, long period) {
    hostmot2_t *hm2 = void_hm2;


    // if there are comm problems, wait for the user to fix it
    if ((*hm2->llio->io_error) != 0) return;

    // if the watchdog has bit, wait for the user to reset it
    if ((hm2->watchdog.num_instances == 0) || (*hm2->watchdog.instance[0].hal.pin.has_bit)) return;


    if (hm2->llio->needs_reset) {
        // user has cleared the bit
        INFO("trying to recover from IO error or Watchdog bite\n");

        // reset the watchdog status
        hm2->watchdog.status_reg[0] = 0;

        // write all settings out to the FPGA
        hm2_force_write(hm2);
        if ((*hm2->llio->io_error) != 0) {
            ERR("error recovery failed\n");
            return;
        }
        INFO("error recover successful!\n");

        hm2->llio->needs_reset = 0;
    }


    // reset the watchdog timer
    // FIXME: write just 1 byte
    hm2->llio->write(hm2->llio, hm2->watchdog.reset_addr, hm2->watchdog.reset_reg, (hm2->watchdog.num_instances * sizeof(u32)));


    // see if we've been bit
    hm2->llio->read(hm2->llio, hm2->watchdog.status_addr, hm2->watchdog.status_reg, (hm2->watchdog.num_instances * sizeof(u32)));
    if ((*hm2->llio->io_error) != 0) return;
    if (hm2->watchdog.status_reg[0] & 0x1) {
        WARN("Watchdog has bit!\n");
        *hm2->watchdog.instance[0].hal.pin.has_bit = 1;
        hm2->llio->needs_reset = 1;
    }
}


int hm2_watchdog_parse_md(hostmot2_t *hm2, int md_index) {
    hm2_module_descriptor_t *md = &hm2->md[md_index];
    int r;

    if (!hm2_md_is_consistent(hm2, md_index, 0, 3, 4, 0)) {
        ERR("inconsistent Module Descriptor!\n");
        return -EINVAL;
    }

    if (hm2->watchdog.num_instances != 0) {
        ERR(
            "Module Descriptor contains duplicate %s (inconsistent firmware), not loading driver\n",
            hm2_get_general_function_name(md->gtag)
        );
        return -EINVAL;
    }

    if (md->instances != 1) {
        WARN("MD declares %d watchdogs!  only using the first one...\n", md->instances);
    }

    hm2->watchdog.num_instances = 1;

    hm2->watchdog.instance = (hm2_watchdog_instance_t *)hal_malloc(hm2->watchdog.num_instances * sizeof(hm2_watchdog_instance_t));
    if (hm2->watchdog.instance == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }

    hm2->watchdog.clock_frequency = md->clock_freq;
    hm2->watchdog.version = md->version;

    hm2->watchdog.timer_addr = md->base_address + (0 * md->register_stride);
    hm2->watchdog.status_addr = md->base_address + (1 * md->register_stride);
    hm2->watchdog.reset_addr = md->base_address + (2 * md->register_stride);


    // 
    // allocate memory for register buffers
    //

    hm2->watchdog.status_reg = (u32 *)kmalloc(hm2->watchdog.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->watchdog.status_reg == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }

    hm2->watchdog.reset_reg = (u32 *)kmalloc(hm2->watchdog.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->watchdog.reset_reg == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail1;
    }

    hm2->watchdog.timer_reg = (u32 *)kmalloc(hm2->watchdog.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->watchdog.timer_reg == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail2;
    }


    //
    // export to HAL
    //

    // pins
    r = hal_pin_bit_newf(
        HAL_IO,
        &(hm2->watchdog.instance[0].hal.pin.has_bit),
        hm2->llio->comp_id,
        "%s.watchdog.has_bit",
        hm2->llio->name
    );
    if (r != HAL_SUCCESS) {
        ERR("error adding pin, aborting\n");
        r = -EINVAL;
        goto fail3;
    }

    // params
    r = hal_param_u32_newf(
        HAL_RW,
        &(hm2->watchdog.instance[0].hal.param.timeout_ns),
        hm2->llio->comp_id,
        "%s.watchdog.timeout_ns",
        hm2->llio->name
    );
    if (r != HAL_SUCCESS) {
        ERR("error adding param, aborting\n");
        r = -EINVAL;
        goto fail3;
    }


    // the function
    {
        char name[HAL_NAME_LEN + 2];

        rtapi_snprintf(name, HAL_NAME_LEN, "%s.pet_watchdog", hm2->llio->name);
        r = hal_export_funct(name, hm2_pet_watchdog, hm2, 0, 0, hm2->llio->comp_id);
        if (r != 0) {
            ERR("error %d exporting pet_watchdog function %s\n", r, name);
            r = -EINVAL;
            goto fail3;
        }
    }


    //
    // initialize the watchdog
    //

    *hm2->watchdog.instance[0].hal.pin.has_bit = 0;
    hm2->watchdog.instance[0].hal.param.timeout_ns = 1000 * 1000 * 1000;  // default timeout is 1 second

    hm2->watchdog.reset_reg[0] = 0x5a000000;
    hm2->watchdog.status_reg[0] = 0;


    return hm2->watchdog.num_instances;


fail3:
    kfree(hm2->watchdog.timer_reg);

fail2:
    kfree(hm2->watchdog.reset_reg);

fail1:
    kfree(hm2->watchdog.status_reg);

fail0:
    hm2->encoder.num_instances = 0;
    return r;
}


void hm2_watchdog_print_module(int msg_level, hostmot2_t *hm2) {
    int i;
    PRINT(msg_level, "Watchdog: %d\n", hm2->watchdog.num_instances);
    if (hm2->watchdog.num_instances <= 0) return;
    PRINT(msg_level, "    clock_frequency: %d Hz (%s MHz)\n", hm2->watchdog.clock_frequency, hm2_hz_to_mhz(hm2->watchdog.clock_frequency));
    PRINT(msg_level, "    version: %d\n", hm2->watchdog.version);
    PRINT(msg_level, "    timer_addr: 0x%04X\n", hm2->watchdog.timer_addr);
    PRINT(msg_level, "    status_addr: 0x%04X\n", hm2->watchdog.status_addr);
    PRINT(msg_level, "    reset_addr: 0x%04X\n", hm2->watchdog.reset_addr);
    for (i = 0; i < hm2->watchdog.num_instances; i ++) {
        PRINT(msg_level, "    instance %d:\n", i);
        PRINT(msg_level, "        param timeout_ns = %u\n", hm2->watchdog.instance[i].hal.param.timeout_ns);
        PRINT(msg_level, "        pin has_bit = %d\n", (*hm2->watchdog.instance[i].hal.pin.has_bit));
        PRINT(msg_level, "        reg timer = 0x%08X\n", hm2->watchdog.timer_reg[i]);
    }
}


void hm2_watchdog_cleanup(hostmot2_t *hm2) {
    if (hm2->watchdog.num_instances == 0) return;
    if (hm2->watchdog.status_reg != NULL) kfree(hm2->watchdog.status_reg);
    if (hm2->watchdog.reset_reg != NULL) kfree(hm2->watchdog.reset_reg);
    if (hm2->watchdog.timer_reg != NULL) kfree(hm2->watchdog.timer_reg);
}


// timeout_s = (timer_counts + 1) / clock_hz
// (timeout_s * clock_hz) - 1 = timer_counts
// (timeout_ns * (1 s/1e9 ns) * clock_hz) - 1 = timer_counts
void hm2_watchdog_force_write(hostmot2_t *hm2) {
    u64 tmp;

    tmp = (hm2->watchdog.instance[0].hal.param.timeout_ns * ((double)hm2->watchdog.clock_frequency / (double)(1000 * 1000 * 1000))) - 1;
    if (tmp < 0x80000000) {
        hm2->watchdog.timer_reg[0] = tmp;
    } else {
        // truncate watchdog timeout
        tmp = 0x7FFFFFFF;
        hm2->watchdog.timer_reg[0] = tmp;
        hm2->watchdog.instance[0].hal.param.timeout_ns = (tmp + 1) / ((double)hm2->watchdog.clock_frequency / (double)(1000 * 1000 * 1000));
        WARN("requested watchdog timeout is out of range, setting it to max: %u ns\n", hm2->watchdog.instance[0].hal.param.timeout_ns);
    }

    // set the watchdog timeout (we'll check for i/o errors later)
    hm2->llio->write(hm2->llio, hm2->watchdog.timer_addr, hm2->watchdog.timer_reg, (hm2->watchdog.num_instances * sizeof(u32)));
    hm2->watchdog.instance[0].written_timeout_ns = hm2->watchdog.instance[0].hal.param.timeout_ns;

    // clear the has-bit bit
    hm2->llio->write(hm2->llio, hm2->watchdog.status_addr, hm2->watchdog.status_reg, sizeof(u32));
}


// if the user has changed the timeout, sync it out to the watchdog
void hm2_watchdog_write(hostmot2_t *hm2) {
    if (hm2->watchdog.num_instances != 1) return;
    if (hm2->watchdog.instance[0].hal.param.timeout_ns == hm2->watchdog.instance[0].written_timeout_ns) return;
    hm2_watchdog_force_write(hm2);
}
