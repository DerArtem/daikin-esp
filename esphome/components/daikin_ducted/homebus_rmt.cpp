/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *                         2022 Greg Davill
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <cinttypes>
#include "esp_check.h"

#include "homebus_rmt.h"

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome
{
  namespace daikin_ducted
  {

    static const char *TAG = "homebus";

    // RMT resolution for homebus, in Hz
    static constexpr uint32_t HOMEBUS_RMT_RESOLUTION_HZ = 4000000UL;

    // Homebus timing parameters (in RMT ticks at HOMEBUS_RMT_RESOLUTION_HZ)
    static constexpr uint32_t HOMEBUS_BIT_RATE = 9600;
    static constexpr uint32_t HOMEBUS_BIT_DURATION = HOMEBUS_RMT_RESOLUTION_HZ / HOMEBUS_BIT_RATE;
    static constexpr uint32_t HOMEBUS_HALF_BIT_DURATION = HOMEBUS_BIT_DURATION / 2;

    // RX buffer size in symbols (enough for 32 bytes * 11 symbols/byte)
    static constexpr size_t RX_BUFFER_SYMBOLS = 48 * 11;

    /*

    Read/Write 1 bit:

              |                BIT_DURATION              | NEXT
              |  HALF_BIT_DURATION  | HALF_BIT_DURATION  | BIT

    ------────────────────────────────────────────────────------




    Read/Write 0 bit:

              |                BIT_DURATION              | NEXT
              |  HALF_BIT_DURATION  | HALF_BIT_DURATION  | BIT

    ------────┐                     ┌─────────────────────------
              │                     │
              │                     │
              │                     │
              └─────────────────────┘

    */

    static void print_packet(const uint8_t *buffer, const uint32_t buffer_length)
    {
      // Each byte needs 3 chars ("xx "), plus prefix (~15) and suffix
      static constexpr size_t MAX_PRINT_BYTES = 48;
      char str[16 + MAX_PRINT_BYTES * 3 + 2];
      char *p = str;

      size_t print_len = (buffer_length > MAX_PRINT_BYTES) ? MAX_PRINT_BYTES : buffer_length;
      p += sprintf(str, "len=%02lu pkt=[", buffer_length);
      for (size_t i = 0; i < print_len; i++)
      {
        p += sprintf(p, "%02x ", buffer[i]);
      }
      if (print_len > 0) {
        *(p - 1) = ']';
      } else {
        *p++ = ']';
      }
      *p = 0;
      ESP_LOGI("p1p2_sniffer", "%s", str);
    }

    static int homebus_rmt_decode_data(rmt_symbol_word_t *rmt_symbols, size_t symbol_num, uint8_t *decoded_bytes, size_t max_buffer_length)
    {
      size_t byte_pos = 0;

      unsigned int symbol_index = 0;
      while (symbol_index < symbol_num)
      {
        // Sample towards the start of a half bit
        unsigned int accumulator = 0;
        unsigned int sampling_time = HOMEBUS_HALF_BIT_DURATION / 3;
        uint16_t decoded_byte = 0;
        size_t bit_pos = 0;
        while (sampling_time < (HOMEBUS_BIT_DURATION * (11)))
        {
          if (symbol_index >= symbol_num)
          {
            ESP_LOGE(TAG, "Invalid symbol index %u >= %zu", symbol_index, symbol_num);
            ESP_LOGE(TAG, "byte_pos = %zu, bit_pos = %zu, accumulator = %u", byte_pos, bit_pos, accumulator);

            return byte_pos;
          }

          rmt_symbol_word_t *symbol = &rmt_symbols[symbol_index];

          if ((symbol_index < symbol_num))
          {

            if (symbol->duration0 > (HOMEBUS_BIT_DURATION * (11)))
            {
              ESP_LOGE(TAG, "Invalid symbol duration0: %u > %" PRIu32, symbol->duration0, (HOMEBUS_BIT_DURATION * (11)));
              return byte_pos;
            }
            if (symbol->duration1 > (HOMEBUS_BIT_DURATION * (11)))
            {
              ESP_LOGE(TAG, "Invalid symbol duration1: %u > %" PRIu32, symbol->duration1, (HOMEBUS_BIT_DURATION * (11)));
              return byte_pos;
            }

            accumulator += symbol->duration0;
            while ((sampling_time < accumulator) && (bit_pos < 11))
            {
              sampling_time += HOMEBUS_BIT_DURATION;
              decoded_byte >>= 1;
              bit_pos++;
            }

            accumulator += symbol->duration1;
          }
          // Pad out accumulator if we've reached the last symbol or timed out
          if ((symbol_index == symbol_num - 1) || (symbol->duration1 == 0))
          {
            accumulator += HOMEBUS_BIT_DURATION * 11;
          }

          while ((sampling_time < accumulator) && (bit_pos < 11))
          {
            sampling_time += HOMEBUS_BIT_DURATION;
            decoded_byte = (decoded_byte >> 1) | (1 << 10);
            bit_pos++;
          }

          symbol_index++;
        }

        /* Check framing and parity */
        if ((decoded_byte & 0x001) == 0x001)
        {
          ESP_LOGE(TAG, "Bad Start framing, %u (byte:0x%03x, idx=%u, len=%zu)", accumulator, decoded_byte, symbol_index, byte_pos);
          print_packet(decoded_bytes, byte_pos);
          return 0;
        }
        if ((decoded_byte & 0x400) != 0x400)
        {
          ESP_LOGE(TAG, "Bad Stop framing, %u (byte:0x%03x, idx=%u, len=%zu)", accumulator, decoded_byte, symbol_index, byte_pos);
          print_packet(decoded_bytes, byte_pos);

          // Only dump symbols that actually exist in the received buffer.
          for (size_t i = 0; i < symbol_num && i < 18; i++)
            ESP_LOGE(TAG, "  symbol[%zu](dur0=%u, dur1=%u)", i,
                     static_cast<unsigned>(rmt_symbols[i].duration0),
                     static_cast<unsigned>(rmt_symbols[i].duration1));
          return 0;
        }
        if ((__builtin_popcount(decoded_byte) & 1) == 0)
        {
          ESP_LOGE(TAG, "Bad parity");
          return 0;
        }

        decoded_byte >>= 1;
        if (byte_pos >= max_buffer_length) {
          ESP_LOGE(TAG, "RX frame exceeds %u bytes", static_cast<unsigned>(max_buffer_length));
          return 0;
        }
        decoded_bytes[byte_pos++] = decoded_byte;
      }
      return byte_pos;
    }


    // ISR-safe function to restart reception on the current write buffer
    static void IRAM_ATTR start_receive_isr(HomebusStore *store)
    {
      rmt_receive_config_t recv_config = {};
      recv_config.signal_range_min_ns = 1000;
      recv_config.signal_range_max_ns = (HOMEBUS_BIT_DURATION * 12) * 1000000000ULL / HOMEBUS_RMT_RESOLUTION_HZ;

      rmt_receive(store->rx_channel, store->buffers[store->write_idx],
                  store->buffer_size * sizeof(rmt_symbol_word_t), &recv_config);
    }

    static bool IRAM_ATTR rmt_rx_done_callback(rmt_channel_handle_t channel,
                                                const rmt_rx_done_event_data_t *edata,
                                                void *user_data)
    {
      HomebusStore *store = (HomebusStore *)user_data;

      if (edata->num_symbols > 0) {
        if (edata->num_symbols <= store->buffer_size) {
          // Mark current write buffer as ready for reading
          store->read_idx = store->write_idx;
          store->num_symbols = edata->num_symbols;
          // Swap to other buffer for next receive
          store->write_idx ^= 1;
        } else {
          store->overflow = true;
        }
      }

      // Immediately restart reception on the next buffer (ISR-safe in ESP-IDF 5.x)
      start_receive_isr(store);

      return false;  // No high priority task woken
    }

    // TX hardware is absent when sniffer_only=true.
    static bool IRAM_ATTR rmt_tx_done_callback(rmt_channel_handle_t channel,
                                                const rmt_tx_done_event_data_t *edata,
                                                void *user_data)
    {
      volatile bool *tx_complete = static_cast<volatile bool *>(user_data);
      *tx_complete = true;
      return false;
    }

    HomebusRMT::~HomebusRMT()
    {
      if (this->tx_encoder_ != nullptr) {
        rmt_del_encoder(this->tx_encoder_);
      }
      if (this->tx_channel_ != nullptr) {
        rmt_disable(this->tx_channel_);
        rmt_del_channel(this->tx_channel_);
      }
      if (this->rx_channel_ != nullptr) {
        rmt_disable(this->rx_channel_);
        rmt_del_channel(this->rx_channel_);
      }
      for (int i = 0; i < 2; i++) {
        if (this->store_.buffers[i] != nullptr) {
          heap_caps_free(this->store_.buffers[i]);
        }
      }
    }

    void HomebusRMT::start_receive_()
    {
      rmt_receive_config_t recv_config = {};
      recv_config.signal_range_min_ns = 1000;  // filter noise < 1us
      recv_config.signal_range_max_ns = (HOMEBUS_BIT_DURATION * 12) * 1000000000ULL / HOMEBUS_RMT_RESOLUTION_HZ;

      // Use current write buffer for reception
      esp_err_t err = rmt_receive(this->rx_channel_,
                                   this->store_.buffers[this->store_.write_idx],
                                   this->store_.buffer_size * sizeof(rmt_symbol_word_t),
                                   &recv_config);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RMT receive: %s", esp_err_to_name(err));
      }
    }

    void HomebusRMT::setup()
    {
      ESP_LOGCONFIG(TAG, "Setting up Homebus...");
      // MAX22088 RST is active low for transmission. HIGH disables TX.
      // Set the safe level before RMT configuration or any allocation.
      gpio_num_t enable_gpio = static_cast<gpio_num_t>(this->enable_pin_);
      gpio_set_level(enable_gpio, 1);
      gpio_config_t enable_conf = {};
      enable_conf.pin_bit_mask = 1ULL << this->enable_pin_;
      enable_conf.mode = GPIO_MODE_OUTPUT;
      enable_conf.pull_up_en = GPIO_PULLUP_ENABLE;
      enable_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      enable_conf.intr_type = GPIO_INTR_DISABLE;
      if (gpio_config(&enable_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MAX22088 RST GPIO");
        this->mark_failed();
        return;
      }
      gpio_set_level(enable_gpio, 1);
      if (this->sniffer_only_) {
        ESP_LOGW(TAG, "READ-ONLY SNIFFER: MAX22088 RST held HIGH, TX hardware not initialized");
      }

      // Allocate RX buffers
      for (int i = 0; i < 2; i++) {
        this->store_.buffers[i] = (rmt_symbol_word_t *)heap_caps_calloc(
            RX_BUFFER_SYMBOLS, sizeof(rmt_symbol_word_t), MALLOC_CAP_8BIT);
        if (this->store_.buffers[i] == nullptr) {
          ESP_LOGE(TAG, "Failed to allocate RX buffer %d", i);
          this->mark_failed();
          return;
        }
      }
      this->store_.buffer_size = RX_BUFFER_SYMBOLS;
      this->store_.write_idx = 0;
      this->store_.read_idx = 0xFF;  // No data ready
      this->store_.overflow = false;
      this->store_.num_symbols = 0;

      // Configure RX channel
      rmt_rx_channel_config_t rx_config = {};
      rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
      rx_config.resolution_hz = HOMEBUS_RMT_RESOLUTION_HZ;
      rx_config.mem_block_symbols = 64;
      rx_config.gpio_num = static_cast<gpio_num_t>(this->rx_pin_);
      rx_config.flags.invert_in = 0;
      rx_config.flags.with_dma = false;

      esp_err_t error = rmt_new_rx_channel(&rx_config, &this->rx_channel_);
      if (error != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to create RX channel: %s", esp_err_to_name(error));
        this->mark_failed();
        return;
      }

      // Store channel handle in store for ISR access
      this->store_.rx_channel = this->rx_channel_;

      // Register RX done callback
      rmt_rx_event_callbacks_t rx_cbs = {};
      rx_cbs.on_recv_done = rmt_rx_done_callback;
      error = rmt_rx_register_event_callbacks(this->rx_channel_, &rx_cbs, &this->store_);
      if (error != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to register RX callbacks: %s", esp_err_to_name(error));
        this->mark_failed();
        return;
      }

      error = rmt_enable(this->rx_channel_);
      if (error != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(error));
        this->mark_failed();
        return;
      }

      if (!this->sniffer_only_) {
        // Configure TX channel
        rmt_tx_channel_config_t tx_config = {};
        tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
        tx_config.resolution_hz = HOMEBUS_RMT_RESOLUTION_HZ;
        tx_config.mem_block_symbols = 64;
        tx_config.gpio_num = static_cast<gpio_num_t>(this->tx_pin_);
        tx_config.trans_queue_depth = 1;
        tx_config.flags.invert_out = 0;
        tx_config.flags.io_od_mode = false;
        tx_config.flags.io_loop_back = false;

        error = rmt_new_tx_channel(&tx_config, &this->tx_channel_);
        if (error != ESP_OK)
        {
          ESP_LOGE(TAG, "Failed to create TX channel: %s", esp_err_to_name(error));
          this->mark_failed();
          return;
        }

        // TX callback is registered only in explicitly enabled control mode.
        rmt_tx_event_callbacks_t tx_cbs = {};
        tx_cbs.on_trans_done = rmt_tx_done_callback;
        error = rmt_tx_register_event_callbacks(this->tx_channel_, &tx_cbs,
                                                const_cast<bool *>(&this->tx_complete_));
        if (error != ESP_OK)
        {
          ESP_LOGE(TAG, "Failed to register TX callback: %s", esp_err_to_name(error));
          this->mark_failed();
          return;
        }

        // Create copy encoder for raw symbol transmission
        rmt_copy_encoder_config_t encoder_config = {};
        error = rmt_new_copy_encoder(&encoder_config, &this->tx_encoder_);
        if (error != ESP_OK)
        {
          ESP_LOGE(TAG, "Failed to create TX encoder: %s", esp_err_to_name(error));
          this->mark_failed();
          return;
        }

        // Enable both channels
        error = rmt_enable(this->tx_channel_);
        if (error != ESP_OK)
        {
          ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(error));
          this->mark_failed();
          return;
        }

        // Transmit dummy byte to initialize TX line high
        rmt_symbol_word_t tx_items[1] = {};
        tx_items[0].duration0 = HOMEBUS_BIT_DURATION * 12;
        tx_items[0].level0 = 1;
        tx_items[0].duration1 = 1;
        tx_items[0].level1 = 1;

        rmt_transmit_config_t tx_cfg = {};
        tx_cfg.loop_count = 0;
        tx_cfg.flags.eot_level = 1;  // idle high
        esp_err_t idle_start_err =
            rmt_transmit(this->tx_channel_, this->tx_encoder_, tx_items, sizeof(tx_items), &tx_cfg);
        if (idle_start_err != ESP_OK) {
          ESP_LOGE(TAG, "TX idle initialization failed to start: %s", esp_err_to_name(idle_start_err));
          this->mark_failed();
          return;
        }
        esp_err_t idle_tx_err = rmt_tx_wait_all_done(this->tx_channel_, pdMS_TO_TICKS(100));
        if (idle_tx_err != ESP_OK) {
          ESP_LOGE(TAG, "TX idle initialization failed: %s", esp_err_to_name(idle_tx_err));
          this->mark_failed();
          return;
        }


      }

      // Start receiving
      this->start_receive_();

      if (!this->sniffer_only_) {
        gpio_set_level(enable_gpio, 0);  // active low: transmitter enabled only in control mode
      }

      ESP_LOGCONFIG(TAG, "Homebus setup complete");
    }

    void HomebusRMT::write_bytes(const uint8_t *tx_data, uint8_t tx_data_size)
    {
      if (this->sniffer_only_) {
        ESP_LOGE(TAG, "Blocked TX attempt in read-only sniffer mode");
        return;
      }
      if (this->is_failed())
        return;
      if (tx_data == nullptr || tx_data_size == 0 || tx_data_size > MAX_TX_BYTES) {
        ESP_LOGE(TAG, "Invalid TX frame size: %u (max %zu)",
                 static_cast<unsigned>(tx_data_size), MAX_TX_BYTES);
        return;
      }
      if (this->tx_channel_ == nullptr || this->tx_encoder_ == nullptr) {
        ESP_LOGE(TAG, "TX channel not initialized");
        return;
      }
      if (this->tx_pending_ || this->tx_in_progress_) {
        ESP_LOGW(TAG, "TX busy, dropping packet");
        return;
      }

      rmt_symbol_word_t *tx_symbols = this->rmt_tx_buffer_;

      rmt_symbol_word_t homebus_bit0_symbol = {};
      homebus_bit0_symbol.duration0 = HOMEBUS_HALF_BIT_DURATION;
      homebus_bit0_symbol.level0 = 0;
      homebus_bit0_symbol.duration1 = HOMEBUS_HALF_BIT_DURATION;
      homebus_bit0_symbol.level1 = 1;

      rmt_symbol_word_t homebus_bit1_symbol = {};
      homebus_bit1_symbol.duration0 = HOMEBUS_HALF_BIT_DURATION;
      homebus_bit1_symbol.level0 = 1;
      homebus_bit1_symbol.duration1 = HOMEBUS_HALF_BIT_DURATION;
      homebus_bit1_symbol.level1 = 1;

      // encode data
      for (int byte_index = 0; byte_index < tx_data_size; byte_index++)
      {
        int symbol_index = byte_index * 11;

        uint8_t cur_byte = tx_data[byte_index];
        bool parity = 0;

        // Start bit
        tx_symbols[symbol_index++] = homebus_bit0_symbol;

        // Byte
        for (int bit_index = 0; bit_index < 8; bit_index++)
        {
          bool tx_bit = (0x01 & cur_byte);
          parity ^= tx_bit;
          tx_symbols[symbol_index++] = tx_bit ? homebus_bit1_symbol : homebus_bit0_symbol;

          cur_byte >>= 1;
        }

        // Even Parity
        tx_symbols[symbol_index++] = parity ? homebus_bit1_symbol : homebus_bit0_symbol;
        // Stop
        tx_symbols[symbol_index++] = homebus_bit1_symbol;
      }

      // Queue the complete frame; receive processing continues during guard time.
      this->tx_pending_symbols_ = static_cast<size_t>(tx_data_size) * SYMBOLS_PER_BYTE;
      this->tx_queued_at_ = millis();
      this->tx_pending_ = true;
    }

    void HomebusRMT::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Homebus:");
      ESP_LOGCONFIG(TAG, "  RX Pin: GPIO%d", this->rx_pin_);
      ESP_LOGCONFIG(TAG, "  TX Pin: GPIO%d", this->tx_pin_);
      ESP_LOGCONFIG(TAG, "  Enable Pin: GPIO%d", this->enable_pin_);
      ESP_LOGCONFIG(TAG, "  Mode: %s", this->sniffer_only_ ? "READ ONLY (TX disabled)" : "CONTROL (TX enabled)");
    }

    void HomebusRMT::loop()
    {
      if (this->is_failed())
        return;

      // TX is deliberately gated: no TX channel is created in sniffer mode.
      if (!this->sniffer_only_) {
        if (this->tx_in_progress_ && this->tx_complete_) {
          this->tx_in_progress_ = false;
          this->tx_complete_ = false;
        }

        if (this->tx_pending_ && !this->tx_in_progress_ &&
            static_cast<uint32_t>(millis() - this->tx_queued_at_) >= TX_GUARD_DELAY_MS) {
          if (this->tx_channel_ == nullptr || this->tx_encoder_ == nullptr) {
            ESP_LOGE(TAG, "TX hardware unavailable, dropping queued packet");
          } else {
            rmt_transmit_config_t tx_cfg = {};
            tx_cfg.loop_count = 0;
            tx_cfg.flags.eot_level = 1;  // idle high

            // Clear BEFORE rmt_transmit, avoiding a missed fast completion.
            this->tx_complete_ = false;
            esp_err_t err = rmt_transmit(this->tx_channel_, this->tx_encoder_,
                                         this->rmt_tx_buffer_,
                                         this->tx_pending_symbols_ * sizeof(rmt_symbol_word_t),
                                         &tx_cfg);
            if (err != ESP_OK) {
              ESP_LOGE(TAG, "TX transmit failed: %s", esp_err_to_name(err));
            } else {
              this->tx_in_progress_ = true;
            }
          }
          this->tx_pending_ = false;
        }
      }

      if (this->store_.overflow) {
        ESP_LOGW(TAG, "RX buffer overflow");
        this->store_.overflow = false;
        return;
      }

      // Check if a buffer is ready for processing
      uint8_t ready_idx = this->store_.read_idx;
      if (ready_idx == 0xFF) {
        return;
      }

      // Capture state and mark as processed
      // Note: Reception already restarted in ISR callback on the other buffer
      size_t num_symbols = this->store_.num_symbols;
      this->store_.read_idx = 0xFF;
      this->store_.num_symbols = 0;

      // Process received data from the completed buffer
      uint8_t buffer[48] = {0};
      size_t decoded_size = homebus_rmt_decode_data(this->store_.buffers[ready_idx], num_symbols, buffer, sizeof(buffer));

      // Filter for potential noise/invalid packets. We need at least the header present
      if (decoded_size < 3)
        return;

      // Packet lines include the local millis timestamp supplied by ESPHome
      // and a monotonically increasing frame counter for offline correlation.
      static uint32_t packet_sequence = 0;
      ESP_LOGI("p1p2_sniffer", "seq=%lu ms=%lu len=%u dir=0x%02X addr=0x%02X type=0x%02X",
               static_cast<unsigned long>(++packet_sequence),
               static_cast<unsigned long>(millis()),
               static_cast<unsigned int>(decoded_size),
               buffer[0], buffer[1], buffer[2]);
      print_packet(buffer, decoded_size);

      // Pass buffer up stack
      if (this->callback_)
      {
        this->callback_(this->callback_arg_, buffer, decoded_size);
      }
    }

  } // namespace daikin_ducted
} // namespace esphome
