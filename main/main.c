/* Flash multiple partitions example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sys/param.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp32_port.h"
#include "esp_loader.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_types.h"
#include <dirent.h>

#define EXAMPLE_MAX_CHAR_SIZE    64


#define MOUNT_POINT "/sdcard"

// Pin assignments can be set in menuconfig, see "SD SPI Example Configuration" menu.
// You can also change the pin assignments here by changing the following 4 lines.
#define PIN_NUM_MISO  CONFIG_EXAMPLE_PIN_MISO
#define PIN_NUM_MOSI  CONFIG_EXAMPLE_PIN_MOSI
#define PIN_NUM_CLK   CONFIG_EXAMPLE_PIN_CLK
#define PIN_NUM_CS    CONFIG_EXAMPLE_PIN_CS

// For esp8266, esp32, esp32s2
#define BOOTLOADER_ADDRESS_V0       0x1000
// For esp32s3 and later chips
#define BOOTLOADER_ADDRESS_V1       0x0
#define PARTITION_ADDRESS           0x8000
#define APPLICATION_ADDRESS         0x10000

static const char *TAG = "serial_flasher";

#define HIGHER_BAUDRATE 230400

static esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

static esp_err_t s_example_read_file(const char *path)
{
    ESP_LOGI(TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE];
    fgets(line, sizeof(line), f);
    fclose(f);

    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);

    return ESP_OK;
}
bool ESPDebug=true;
esp_loader_error_t flash_binary_l(FILE *file, size_t size, unsigned long address)
{

    esp_loader_error_t err;
    uint8_t payload[1024];

    if(ESPDebug) ESP_LOGI(TAG, "Erasing flash (this may take a while)...\n");
    err = esp_loader_flash_start(address, size, sizeof(payload));
    if (err != ESP_LOADER_SUCCESS) {
        if(ESPDebug) ESP_LOGI(TAG, "Erasing flash failed with error : %d",err);
        return err;
    }
    if(ESPDebug) ESP_LOGI(TAG, "Start programming\n");
	if(ESPDebug) ESP_LOGI(TAG, "\rProgress: ");
    size_t binary_size = size;
    size_t written = 0;
    int previousProgress = -1;
    while (size > 0) {
        size_t to_read = MIN(size, sizeof(payload));
        fread(payload,sizeof(uint8_t),to_read,file);
        err = esp_loader_flash_write(payload, to_read);

        if (err != ESP_LOADER_SUCCESS) {
            printf("\nPacket could not be written! Error %d.\n", err);
            return err;
        }

        size -= to_read;
        written += to_read;

        int progress = (int)(((float)written / binary_size) * 100);
        printf("\rProgress: %d %%", progress);
        fflush(stdout);
    };
    if(ESPDebug) ESP_LOGI(TAG, "\nFinished programming\n");

#if MD5_ENABLED
    err = esp_loader_flash_verify();
    if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
        if(ESPDebug) ESP_LOGI(TAG, "ESP8266 does not support flash verify command.");
        return err;
    } else if (err != ESP_LOADER_SUCCESS) {
        if(ESPDebug) ESP_LOGI(TAG, "MD5 does not match. err: %d\n",err);
        return err;
    }
    if(ESPDebug) ESP_LOGI(TAG, "Flash verified\n");
#endif

    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t connect_to_target(uint32_t higher_transmission_rate)
{
    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();

    esp_loader_error_t err = esp_loader_connect(&connect_config);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Cannot connect to target. Error: %u\n", err);
        return err;
    }
    printf("Connected to target\n");

#ifdef SERIAL_FLASHER_INTERFACE_UART
    if (higher_transmission_rate && esp_loader_get_target() != ESP8266_CHIP) {
        err = esp_loader_change_transmission_rate(higher_transmission_rate);
        if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
            printf("ESP8266 does not support change transmission rate command.");
            return err;
        } else if (err != ESP_LOADER_SUCCESS) {
            printf("Unable to change transmission rate on target.");
            return err;
        } else {
            err = loader_port_change_transmission_rate(higher_transmission_rate);
            if (err != ESP_LOADER_SUCCESS) {
                printf("Unable to change transmission rate.");
                return err;
            }
            printf("Transmission rate changed changed\n");
        }
    }
#endif /* SERIAL_FLASHER_INTERFACE_UART */

    return ESP_LOADER_SUCCESS;
}


void app_main(void)
{
    //example_binaries_t bin;

    const loader_esp32_config_t config = {
        .baud_rate = 115200,
        .uart_port = UART_NUM_1,
        .uart_rx_pin = GPIO_NUM_5,
        .uart_tx_pin = GPIO_NUM_4,
        .reset_trigger_pin = GPIO_NUM_25,
        .gpio0_trigger_pin = GPIO_NUM_26,
    };
    esp_err_t ret;

    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz=4000;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    struct dirent *dp;
	DIR *dir = opendir(MOUNT_POINT);
	if (dir == NULL) {
		ESP_LOGE(TAG, "Can't Open Dir.");
	}
	while ((dp = readdir (dir)) != NULL) {
		ESP_LOGI(TAG, "[%s]\n", dp->d_name);
	}
	closedir (dir);


	const char *bootloader_file_name = MOUNT_POINT"/BOOTLO~1.BIN";
	const char *partition_file_name = MOUNT_POINT"/PARTIT~1.BIN";
	const char *firmware_file_name = MOUNT_POINT"/FIRMWARE.BIN";

	ESP_LOGI(TAG, "Opening file %s", bootloader_file_name);
	FILE *bootloader_file = fopen(bootloader_file_name, "r");

	if (bootloader_file != NULL) {

	   fseek(bootloader_file,0,SEEK_END);
	   uint32_t bootloader_size = ftell(bootloader_file);
	   fseek(bootloader_file,0,SEEK_SET);
	   ESP_LOGI(TAG, "Bootloader is %lu bytes long.",bootloader_size);

	   FILE *partition_file = fopen(partition_file_name, "r");

	   if (partition_file != NULL) {

		   fseek(partition_file,0,SEEK_END);
		   uint32_t partition_size = ftell(partition_file);
		   fseek(partition_file,0,SEEK_SET);
		   ESP_LOGI(TAG, "Partition-table is %lu bytes long.",partition_size);

		   FILE *firmware_file = fopen(firmware_file_name, "r");

		   if (firmware_file != NULL) {

			   fseek(firmware_file,0,SEEK_END);
			   uint32_t firmware_size = ftell(firmware_file);
			   fseek(firmware_file,0,SEEK_SET);
			   ESP_LOGI(TAG, "Firmware is %lu bytes long.",firmware_size);

			   if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS) {
				   ESP_LOGE(TAG, "serial initialization failed.");
				   return;
			   }
			   if (connect_to_target(HIGHER_BAUDRATE) == ESP_LOADER_SUCCESS) {


					ESP_LOGI(TAG, "Loading bootloader...");
					flash_binary_l(bootloader_file, bootloader_size, BOOTLOADER_ADDRESS_V0);
					ESP_LOGI(TAG, "Loading partition table...");
					flash_binary_l(partition_file, partition_size, PARTITION_ADDRESS);
					ESP_LOGI(TAG, "Loading app...");
					flash_binary_l(firmware_file,  firmware_size,  APPLICATION_ADDRESS);
					ESP_LOGI(TAG, "Done!");
			   }
			   return;
		   }
		   else{
			   ESP_LOGE(TAG, "Failed to open firmware");
		   }
	   }
	   else{
		   ESP_LOGE(TAG, "Failed to open partition-table");
	   }
	}
	else{
	   ESP_LOGE(TAG, "Failed to open bootloader");
	}


}
