/*
 * eBike tracker — persistent configuration implementation (NVS).
 */

#include "storage.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(storage, CONFIG_EBIKE_LOG_LEVEL);

/* NVS entry ids. Never reuse one: the data outlives reflashes as long as the
 * partition is not erased. */
#define NVS_ID_WHEEL_CIRC_MM        1
#define NVS_ID_ARMED                2
#define NVS_ID_PUBLISH_INTERVAL_S   3
#define NVS_ID_MOTION_THRESHOLD_MG  4
#define NVS_ID_HOME_LAT             5
#define NVS_ID_HOME_LON             6
#define NVS_ID_GEOFENCE_RADIUS_M    7
#define NVS_ID_GEOFENCE_ENABLED     8

#define STORAGE_PARTITION storage_partition

static struct nvs_fs fs = {
	.flash_device = PARTITION_DEVICE(STORAGE_PARTITION),
	.offset = PARTITION_OFFSET(STORAGE_PARTITION),
};

static bool mounted;

int storage_init(void)
{
	struct flash_pages_info page;
	int err;

	if (!device_is_ready(fs.flash_device)) {
		LOG_ERR("Flash %s not ready", fs.flash_device->name);
		return -ENODEV;
	}

	err = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page);
	if (err) {
		LOG_ERR("Flash page info unavailable (%d)", err);
		return err;
	}

	fs.sector_size = page.size;
	fs.sector_count = PARTITION_SIZE(STORAGE_PARTITION) / page.size;

	err = nvs_mount(&fs);
	if (err) {
		LOG_ERR("NVS mount failed (%d)", err);
		return err;
	}

	mounted = true;
	LOG_INF("NVS mounted (%u sectors of %u bytes, offset 0x%lx)",
		fs.sector_count, fs.sector_size, (unsigned long)fs.offset);

	return 0;
}

/* u32 read: the NVS value when present and non-zero, the fallback otherwise. */
static uint32_t get_u32(uint16_t id, uint32_t fallback)
{
	uint32_t val;
	ssize_t len;

	if (!mounted) {
		return fallback;
	}

	len = nvs_read(&fs, id, &val, sizeof(val));
	if (len != sizeof(val) || val == 0) {
		return fallback;
	}

	return val;
}

/* u32 write. nvs_write returns the size written, 0 when the value is
 * unchanged (no needless flash write), negative on error. */
static int set_u32(uint16_t id, uint32_t val)
{
	ssize_t len;

	if (!mounted) {
		return -EACCES;
	}

	len = nvs_write(&fs, id, &val, sizeof(val));
	if (len < 0) {
		LOG_ERR("NVS write of id %u failed (%d)", id, (int)len);
		return (int)len;
	}

	return 0;
}

static bool get_flag(uint16_t id)
{
	uint8_t val;

	if (!mounted) {
		return false;
	}

	return nvs_read(&fs, id, &val, sizeof(val)) == sizeof(val) && val != 0;
}

static int set_flag(uint16_t id, bool on)
{
	uint8_t val = on ? 1 : 0;
	ssize_t len;

	if (!mounted) {
		return -EACCES;
	}

	len = nvs_write(&fs, id, &val, sizeof(val));
	if (len < 0) {
		LOG_ERR("NVS write of id %u failed (%d)", id, (int)len);
		return (int)len;
	}

	return 0;
}

uint32_t storage_get_wheel_circ_mm(void)
{
	return get_u32(NVS_ID_WHEEL_CIRC_MM, CONFIG_EBIKE_WHEEL_CIRC_MM);
}

int storage_set_wheel_circ_mm(uint32_t circ_mm)
{
	return set_u32(NVS_ID_WHEEL_CIRC_MM, circ_mm);
}

bool storage_get_armed(void)
{
	return get_flag(NVS_ID_ARMED);
}

int storage_set_armed(bool armed)
{
	return set_flag(NVS_ID_ARMED, armed);
}

uint32_t storage_get_publish_interval_s(void)
{
	return get_u32(NVS_ID_PUBLISH_INTERVAL_S, CONFIG_EBIKE_PUBLISH_RIDING_S);
}

int storage_set_publish_interval_s(uint32_t seconds)
{
	return set_u32(NVS_ID_PUBLISH_INTERVAL_S, seconds);
}

uint32_t storage_get_motion_threshold_mg(void)
{
	return get_u32(NVS_ID_MOTION_THRESHOLD_MG,
		       CONFIG_EBIKE_MOTION_THRESHOLD_MG);
}

int storage_set_motion_threshold_mg(uint32_t mg)
{
	return set_u32(NVS_ID_MOTION_THRESHOLD_MG, mg);
}

bool storage_get_home(double *lat, double *lon)
{
	double v_lat, v_lon;

	if (!mounted) {
		return false;
	}

	if (nvs_read(&fs, NVS_ID_HOME_LAT, &v_lat, sizeof(v_lat)) !=
		    sizeof(v_lat) ||
	    nvs_read(&fs, NVS_ID_HOME_LON, &v_lon, sizeof(v_lon)) !=
		    sizeof(v_lon)) {
		return false;
	}

	*lat = v_lat;
	*lon = v_lon;

	return true;
}

int storage_set_home(double lat, double lon)
{
	ssize_t len;

	if (!mounted) {
		return -EACCES;
	}

	len = nvs_write(&fs, NVS_ID_HOME_LAT, &lat, sizeof(lat));
	if (len < 0) {
		LOG_ERR("NVS write of home_lat failed (%d)", (int)len);
		return (int)len;
	}

	len = nvs_write(&fs, NVS_ID_HOME_LON, &lon, sizeof(lon));
	if (len < 0) {
		LOG_ERR("NVS write of home_lon failed (%d)", (int)len);
		return (int)len;
	}

	return 0;
}

uint32_t storage_get_geofence_radius_m(void)
{
	return get_u32(NVS_ID_GEOFENCE_RADIUS_M, CONFIG_EBIKE_GEOFENCE_RADIUS_M);
}

int storage_set_geofence_radius_m(uint32_t radius_m)
{
	return set_u32(NVS_ID_GEOFENCE_RADIUS_M, radius_m);
}

bool storage_get_geofence_enabled(void)
{
	return get_flag(NVS_ID_GEOFENCE_ENABLED);
}

int storage_set_geofence_enabled(bool enabled)
{
	return set_flag(NVS_ID_GEOFENCE_ENABLED, enabled);
}
