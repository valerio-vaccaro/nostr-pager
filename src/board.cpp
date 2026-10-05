#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include "panel_init.h"

namespace {
spi_device_handle_t panel;
lv_disp_draw_buf_t drawBuffer;
uint16_t *frame, *dma;
constexpr size_t ChunkPixels = 2048;

void command(uint8_t cmd, const uint8_t *data = nullptr, size_t length = 0) {
    spi_transaction_t t{};
    t.cmd = 0x02;
    t.addr = uint32_t(cmd) << 8;
    t.length = length * 8;
    t.tx_buffer = data;
    digitalWrite(board::Cs, LOW);
    ESP_ERROR_CHECK(spi_device_polling_transmit(panel, &t));
    digitalWrite(board::Cs, HIGH);
}

// The AXS15231B needs a contiguous full frame; LVGL partial areas update PSRAM.
void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    const int width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(frame + y * board::Width + area->x1, colors, width * 2);
        colors += width;
    }
    if (lv_disp_flush_is_last(driver)) {
        const uint8_t x[] = {0, 0, 1, 0x3f}, y[] = {0, 0, 1, 0xdf};
        command(0x2a, x, 4);
        command(0x2b, y, 4);
        command(0x2c);
        digitalWrite(board::Cs, LOW);
        for (size_t offset = 0; offset < board::Width * board::Height;) {
            size_t count = std::min(ChunkPixels, size_t(board::Width * board::Height) - offset);
            for (size_t i = 0; i < count; ++i) dma[i] = __builtin_bswap16(frame[offset + i]);
            spi_transaction_ext_t t{};
            t.base.flags = SPI_TRANS_MODE_QIO;
            if (offset == 0) { t.base.cmd = 0x32; t.base.addr = 0x003c00; }
            else t.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
            t.base.length = count * 16;
            t.base.tx_buffer = dma;
            ESP_ERROR_CHECK(spi_device_polling_transmit(panel, &t.base));
            offset += count;
        }
        digitalWrite(board::Cs, HIGH);
    }
    lv_disp_flush_ready(driver);
}

void touch(lv_indev_drv_t *, lv_indev_data_t *out) {
    out->state = LV_INDEV_STATE_RELEASED;
    const uint8_t request[] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 8, 0, 0, 0};
    Wire.beginTransmission(0x3b);
    Wire.write(request, sizeof(request));
    if (Wire.endTransmission() != 0) return;
    if (Wire.requestFrom(uint8_t(0x3b), uint8_t(8)) != 8) return;
    uint8_t data[8];
    for (auto &b : data) b = Wire.read();
    if (data[0] != 0 || data[1] != 1 || (data[2] & 0xc0)) return;
    int x = ((data[2] & 15) << 8) | data[3];
    int y = ((data[4] & 15) << 8) | data[5];
    if (x >= board::Width || y >= board::Height) return;
    out->point = {lv_coord_t(x), lv_coord_t(y)};
    out->state = LV_INDEV_STATE_PRESSED;
}
}

bool board::begin() {
    if (!psramFound()) return false;
    frame = static_cast<uint16_t *>(ps_calloc(Width * Height, 2));
    dma = static_cast<uint16_t *>(heap_caps_malloc(ChunkPixels * 2, MALLOC_CAP_DMA));
    auto *buf = static_cast<lv_color_t *>(heap_caps_malloc(Width * 24 * 2, MALLOC_CAP_INTERNAL));
    if (!frame || !dma || !buf) return false;
    pinMode(Backlight, OUTPUT);
    digitalWrite(Backlight, LOW);
    pinMode(Cs, OUTPUT);
    digitalWrite(Cs, HIGH);
    spi_bus_config_t bus{};
    bus.sclk_io_num = Clock; bus.mosi_io_num = D0; bus.miso_io_num = D1;
    bus.quadwp_io_num = D2; bus.quadhd_io_num = D3;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = ChunkPixels * 2;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    spi_device_interface_config_t device{};
    device.command_bits = 8; device.address_bits = 24;
    device.clock_speed_hz = 32000000;
    device.spics_io_num = -1; device.flags = SPI_DEVICE_HALFDUPLEX; device.queue_size = 1;
    if (spi_bus_add_device(SPI2_HOST, &device, &panel) != ESP_OK) return false;
    command(0x01);
    delay(200);
    for (size_t i = 0; i < sizeof(panelInit);) {
        uint8_t cmd = panelInit[i++], n = panelInit[i++];
        if (cmd == 0xff) delay(n);
        else { command(cmd, panelInit + i, n); i += n; }
    }
    const uint8_t pixelFormat = 0x55, orientation = 0;
    command(0x3a, &pixelFormat, 1);
    command(0x36, &orientation, 1);
    Wire.begin(TouchSda, TouchScl, 400000);
    Wire.setTimeOut(20);
    pinMode(TouchInterrupt, INPUT);
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, buf, nullptr, Width * 24);
    static lv_disp_drv_t display;
    lv_disp_drv_init(&display);
    display.hor_res = Width; display.ver_res = Height;
    display.flush_cb = flush; display.draw_buf = &drawBuffer;
    lv_disp_drv_register(&display);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input);
    input.type = LV_INDEV_TYPE_POINTER; input.read_cb = touch;
    lv_indev_drv_register(&input);
    digitalWrite(Backlight, HIGH);
    return true;
}
