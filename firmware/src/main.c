/*
 * eBike tracker — nRF9161.
 *
 * Bring-up skeleton: prints a heartbeat so the board, the flash layout and
 * the serial console can be validated before any radio or sensor code is
 * added.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ebike, CONFIG_EBIKE_LOG_LEVEL);

int main(void)
{
	unsigned int beat = 0;

	printk("\n>>> eBike: main() started <<<\n");
	LOG_INF("eBike firmware — board: %s", CONFIG_BOARD);

	while (1) {
		LOG_INF("alive — heartbeat #%u", ++beat);
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
