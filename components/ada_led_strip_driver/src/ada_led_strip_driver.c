#include "led_strip.h"
#include "esp_log.h"
#include "esp_err.h"
#include "stdbool.h"
#include "esp_timer.h"

#include "ada_led_strip_driver.h"

static const char *TAG = "ADA_LED_STRIP_DRIVER";

static led_strip_handle_t led_strip;

// Flag to signal the task to exit gracefully
static volatile bool led_effect_running = false;
static TaskHandle_t led_effect_task_handle = NULL;

static SemaphoreHandle_t led_strip_mutex = NULL;

// static led_effect_finished_cb_t effect_finished_cb = NULL;

typedef struct
{
    uint16_t start_led;
    uint8_t target_r;
    uint8_t target_g;
    uint8_t target_b;
    uint8_t fade_steps;
    uint32_t step_delay_ms;
    uint32_t led_delay_ms;
    bool reverse;
    uint32_t expected_duration_ms;
    uint8_t breath_cycles;
} effect_params_t;

/**
 * @brief Configure and initialize the LED strip
 *
 * @return esp_err_t ESP_OK on success, otherwise error code
 */
esp_err_t configure_led_strip(void)
{
    ESP_LOGI(TAG, "Initializing LED strip on GPIO %d", CONFIG_ADA_LED_STRIP_GPIO);

    if (led_strip_mutex == NULL)
    {
        led_strip_mutex = xSemaphoreCreateMutex();
        if (led_strip_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create LED strip mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_ADA_LED_STRIP_GPIO,
        .max_leds = CONFIG_ADA_LED_STRIP_MAX_LEDS // at least one LED on board
    };

#if CONFIG_ADA_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

#elif CONFIG_ADA_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));

#else
#error "unsupported LED strip backend"
#endif

    // Take the mutex before initial operation
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take LED mutex for initial clear");
        return ESP_FAIL;
    }

    /* Set all LED off to clear all pixels */
    esp_err_t clear_result = led_strip_clear(led_strip);
    xSemaphoreGive(led_strip_mutex);

    return clear_result;
}

/**
 * @brief HSV to RGB conversion function for LED animation
 *
 * @param h Hue (0-359)
 * @param s Saturation (0-100)
 * @param v Value/Brightness (0-100)
 * @return uint32_t RGB color value
 */
static uint32_t led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    uint32_t r, g, b;

    switch (i)
    {
    case 0:
        r = rgb_max;
        g = rgb_min + rgb_adj;
        b = rgb_min;
        break;
    case 1:
        r = rgb_max - rgb_adj;
        g = rgb_max;
        b = rgb_min;
        break;
    case 2:
        r = rgb_min;
        g = rgb_max;
        b = rgb_min + rgb_adj;
        break;
    case 3:
        r = rgb_min;
        g = rgb_max - rgb_adj;
        b = rgb_max;
        break;
    case 4:
        r = rgb_min + rgb_adj;
        g = rgb_min;
        b = rgb_max;
        break;
    default:
        r = rgb_max;
        g = rgb_min;
        b = rgb_max - rgb_adj;
        break;
    }

    return (r << 16) | (g << 8) | b;
}

/**
 * @brief Task function for rainbow animation
 *
 * This task safely checks led_effect_running flag before each iteration
 * to support clean shutdown
 */
static void rainbow_effect_Task(void *arg)
{
    uint8_t hue = 0;
    esp_err_t result;

    while (led_effect_running)
    {
        // Try to get mutex access to the LED strip
        if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Failed to take LED mutex in rainbow task");
            // If we can't get the mutex, wait a bit and try again
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        // Check again if we should still be running after acquiring the mutex
        if (!led_effect_running)
        {
            xSemaphoreGive(led_strip_mutex);
            break;
        }

        // Animate the LED strip to show a moving rainbow effect
        for (int i = 0; i < CONFIG_ADA_LED_STRIP_MAX_LEDS; i++)
        {
            // Calculate hue based on position and time for rainbow effect
            // Adding hue to the position creates the movement effect
            uint16_t current_hue = (hue + i * 30) % 360;

            // Convert HSV (hue, 100% saturation, 20% brightness) to RGB
            uint32_t rgb = led_strip_hsv2rgb(current_hue, 100, 20);

            // Set the RGB values for the LED
            led_strip_set_pixel(led_strip, i, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }

        // Increment hue for the next cycle to create animation movement
        hue = (hue + 5) % 360;

        // Refresh the strip to send data
        result = led_strip_refresh(led_strip);
        if (result != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(result));
        }

        // Release the mutex before delay
        xSemaphoreGive(led_strip_mutex);

        vTaskDelay(50 / portTICK_PERIOD_MS); // Adjust delay for animation speed
    }

    // Final cleanup before exiting task
    ESP_LOGI(TAG, "Rainbow effect task exiting");
    vTaskDelete(NULL);
}

esp_err_t ada_led_strip_sequential_fade_in(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint8_t fade_steps,
    uint32_t step_delay_ms,
    uint32_t led_delay_ms,
    bool reverse)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when setting color fade in");
        return ESP_ERR_TIMEOUT;
    }

    if (fade_steps == 0)
    {
        fade_steps = 1;
    }

    int num_leds = CONFIG_ADA_LED_STRIP_MAX_LEDS;

    // Array to track current brightness level of each LED
    uint8_t *led_brightness = (uint8_t *)calloc(num_leds, sizeof(uint8_t));
    if (led_brightness == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    // Keep track of how many LEDs have completed their fade-in
    uint16_t completed_leds = 0;

    // Variable to track the next LED to start fading
    uint16_t next_led_to_start = 0;

    // Time tracking for LED delays
    uint32_t last_led_start_time = 0;
    uint32_t current_time = 0;

    led_effect_running = true;

    // Continue until all LEDs have completed their fade-in
    while (completed_leds < num_leds && led_effect_running)
    {
        // Check if it's time to start a new LED fading
        if (next_led_to_start < num_leds && (current_time - last_led_start_time >= led_delay_ms || next_led_to_start == 0))
        {
            // Mark this LED as active (brightness = 1)
            led_brightness[next_led_to_start] = 1;
            next_led_to_start++;
            last_led_start_time = current_time;
        }

        // Update all active LEDs
        for (uint16_t i = 0; i < num_leds; i++)
        {
            if (led_brightness[i] > 0 && led_brightness[i] <= fade_steps)
            {
                // Calculate the current brightness percentage (0.0 to 1.0)
                float brightness = (float)led_brightness[i] / fade_steps;

                // Calculate the current RGB values
                uint8_t current_r = (uint8_t)(target_r * brightness);
                uint8_t current_g = (uint8_t)(target_g * brightness);
                uint8_t current_b = (uint8_t)(target_b * brightness);

                // Update this LED
                uint16_t led_index = reverse ? (start_led + num_leds - 1 - i) : (start_led + i);
                led_strip_set_pixel(led_strip, led_index, current_r, current_g, current_b);

                // Increment brightness for next step
                led_brightness[i]++;

                // Check if this LED has completed its fade
                if (led_brightness[i] > fade_steps)
                {
                    completed_leds++;
                }
            }
        }

        // Refresh the LED strip to show the changes
        led_strip_refresh(led_strip);

        // Wait before the next update
        vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
        current_time += step_delay_ms;
    }

    // Free the allocated memory
    free(led_brightness);

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return result;
}

static void ada_led_strip_sequential_fade_in_task(void *arg)
{
    effect_params_t *params = (effect_params_t *)arg;

    // Local variables for clarity and to handle task cleanup
    uint16_t start_led = params->start_led;
    uint8_t target_r = params->target_r;
    uint8_t target_g = params->target_g;
    uint8_t target_b = params->target_b;
    uint8_t fade_steps = params->fade_steps;
    uint32_t step_delay_ms = params->step_delay_ms;
    uint32_t led_delay_ms = params->led_delay_ms;
    bool reverse = params->reverse;
    uint32_t expected_duration_ms = params->expected_duration_ms;

    // Free the parameters structure as we've copied the values
    free(params);

    uint32_t effect_start_time = esp_timer_get_time() / 1000;

    ada_led_strip_sequential_fade_in(
        start_led,
        target_r,
        target_g,
        target_b,
        fade_steps,
        step_delay_ms,
        led_delay_ms,
        reverse);

    uint32_t current_time = esp_timer_get_time() / 1000;
    uint32_t led_effect_duration = current_time - effect_start_time;

    ESP_LOGI(TAG, "Sequential fade in completed");
    ESP_LOGI(TAG, "Expected duration: %lu ms, Actual duration: %lu ms", expected_duration_ms, led_effect_duration);

    uint32_t difference = (led_effect_duration > expected_duration_ms)
                              ? (led_effect_duration - expected_duration_ms)
                              : (expected_duration_ms - led_effect_duration);

    if (difference > (expected_duration_ms / 20) && led_effect_running)
    {
        ESP_LOGW(TAG, "Effect timing was significantly off: %lu ms", difference);
    }

    // Mark task as complete
    led_effect_running = false;
    led_effect_task_handle = NULL;

    // Delete this task
    vTaskDelete(NULL);
}

/**
 * Time-based wrapper for the sequential fade-in animation
 * Calculates optimal parameters to achieve the desired animation duration
 * Each LED fades in over the same duration with evenly staggered start times
 *
 * @param start_led         First LED in the strip to animate
 * @param target_r          Target red component (0-255)
 * @param target_g          Target green component (0-255)
 * @param target_b          Target blue component (0-255)
 * @param total_duration_ms Total time for the entire animation in milliseconds
 * @param reverse           If true, LEDs will animate in reverse order
 */
esp_err_t ada_led_strip_start_sequential_fade_in_with_duration(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint32_t total_duration_ms,
    bool reverse)
{
    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when starting sequential fade");
        return ESP_ERR_TIMEOUT;
    }

    // Check if task is already running
    if (led_effect_task_handle != NULL || led_effect_running)
    {
        ESP_LOGW(TAG, "Effect already running. Stop it first.");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    int num_leds = CONFIG_ADA_LED_STRIP_MAX_LEDS;

    // Apply a reasonable minimum duration
    uint32_t min_duration_ms = num_leds * 10; // At least 10ms per LED
    if (total_duration_ms < min_duration_ms)
    {
        total_duration_ms = min_duration_ms;
    }

    // Each LED will have the same fade-in time
    // We want each LED to completely fade in over a fixed duration
    uint8_t fade_steps = 20; // Fixed number of steps for smooth fade

    // Calculate the fade time for each individual LED
    // Using 40% of total duration as the fade time for each LED
    uint32_t individual_fade_time = total_duration_ms * 2 / 5;

    // Calculate step delay (time between brightness increments)
    uint32_t step_delay_ms = individual_fade_time / fade_steps;
    if (step_delay_ms < 5)
    {
        step_delay_ms = 5; // Minimum delay of 5ms
        // Recalculate individual_fade_time based on minimum step delay
        individual_fade_time = step_delay_ms * fade_steps;
    }

    // Calculate LED delay (time between starting each LED's fade)
    // Spread the remaining 60% of time across all LEDs
    uint32_t led_delay_ms = (total_duration_ms - individual_fade_time) / (num_leds - 1);
    if (led_delay_ms < 5)
    {
        led_delay_ms = 5; // Minimum delay of 5ms
        // Recalculate total_duration if needed to maintain consistency
        total_duration_ms = individual_fade_time + led_delay_ms * (num_leds - 1);
    }

    // Calculate actual expected duration based on our parameters
    uint32_t calculated_duration = led_delay_ms * (num_leds - 1) + individual_fade_time;

    ESP_LOGI(TAG, "Sequential fade: requested=%lums, calculated=%lums, step_delay=%lums, led_delay=%lums",
             total_duration_ms, calculated_duration, step_delay_ms, led_delay_ms);

    // Allocate and populate parameters for the task
    effect_params_t *params = malloc(sizeof(effect_params_t));
    if (params == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for fade task parameters");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_NO_MEM;
    }

    params->start_led = start_led;
    params->target_r = target_r;
    params->target_g = target_g;
    params->target_b = target_b;
    params->fade_steps = fade_steps;
    params->step_delay_ms = step_delay_ms;
    params->led_delay_ms = led_delay_ms;
    params->reverse = reverse;
    params->expected_duration_ms = total_duration_ms;

    // Set flag before creating task
    led_effect_running = true;

    // Release mutex before creating task
    xSemaphoreGive(led_strip_mutex);

    // Create the animation task
    BaseType_t task_created = xTaskCreate(
        ada_led_strip_sequential_fade_in_task,
        "ada_led_strip_sequential_fade_in_task",
        4096,
        params,
        5,
        &led_effect_task_handle);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create sequential fade task");
        free(params);

        // Reset flags if task creation failed
        if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            led_effect_running = false;
            xSemaphoreGive(led_strip_mutex);
        }

        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ada_led_strip_sequential_fade_out(
    uint16_t start_led,
    uint8_t start_r,
    uint8_t start_g,
    uint8_t start_b,
    uint8_t fade_steps,
    uint32_t step_delay_ms,
    uint32_t led_delay_ms,
    bool reverse)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when setting color fade out");
        return ESP_ERR_TIMEOUT;
    }

    if (fade_steps == 0)
    {
        fade_steps = 1;
    }

    int num_leds = CONFIG_ADA_LED_STRIP_MAX_LEDS;

    // Array to track current brightness level of each LED
    uint8_t *led_brightness = (uint8_t *)calloc(num_leds, sizeof(uint8_t));
    if (led_brightness == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    // Initialize all LEDs to full brightness
    for (uint16_t i = 0; i < num_leds; i++)
    {
        led_brightness[i] = fade_steps;
    }

    // Keep track of how many LEDs have completed their fade-out
    uint16_t completed_leds = 0;

    // Variable to track the next LED to start fading out
    uint16_t next_led_to_start = 0;

    // Time tracking for LED delays
    uint32_t last_led_start_time = 0;
    uint32_t current_time = 0;

    led_effect_running = true;

    // Continue until all LEDs have completed their fade-out
    while (completed_leds < num_leds && led_effect_running)
    {
        // Check if it's time to start a new LED fading out
        if (next_led_to_start < num_leds && (current_time - last_led_start_time >= led_delay_ms || next_led_to_start == 0))
        {
            // Mark this LED as active (start the fade out)
            next_led_to_start++;
            last_led_start_time = current_time;
        }

        // Update all active LEDs
        for (uint16_t i = 0; i < next_led_to_start; i++)
        {
            if (led_brightness[i] > 0)
            {
                // Decrement brightness for next step
                led_brightness[i]--;

                // Calculate the current brightness percentage (0.0 to 1.0)
                float brightness = (float)led_brightness[i] / fade_steps;

                // Calculate the current RGB values
                uint8_t current_r = (uint8_t)(start_r * brightness);
                uint8_t current_g = (uint8_t)(start_g * brightness);
                uint8_t current_b = (uint8_t)(start_b * brightness);

                // Update this LED
                if (reverse)
                {
                    led_strip_set_pixel(led_strip, start_led - i, current_r, current_g, current_b);
                }
                else
                {
                    led_strip_set_pixel(led_strip, start_led + i, current_r, current_g, current_b);
                }

                // Check if this LED has completed its fade
                if (led_brightness[i] == 0)
                {
                    completed_leds++;
                }
            }
        }

        // Refresh the LED strip to show the changes
        led_strip_refresh(led_strip);

        // Wait before the next update
        vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
        current_time += step_delay_ms;
    }

    // Free the allocated memory
    free(led_brightness);

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return result;
}

static void ada_led_strip_sequential_fade_out_task(void *arg)
{
    effect_params_t *params = (effect_params_t *)arg;

    // Local variables for clarity and to handle task cleanup
    uint16_t start_led = params->start_led;
    uint8_t start_r = params->target_r; // Reusing target_r as start_r
    uint8_t start_g = params->target_g; // Reusing target_g as start_g
    uint8_t start_b = params->target_b; // Reusing target_b as start_b
    uint8_t fade_steps = params->fade_steps;
    uint32_t step_delay_ms = params->step_delay_ms;
    uint32_t led_delay_ms = params->led_delay_ms;
    bool reverse = params->reverse;
    uint32_t expected_duration_ms = params->expected_duration_ms;

    // Free the parameters structure as we've copied the values
    free(params);

    uint32_t effect_start_time = esp_timer_get_time() / 1000;

    ada_led_strip_sequential_fade_out(
        start_led,
        start_r,
        start_g,
        start_b,
        fade_steps,
        step_delay_ms,
        led_delay_ms,
        reverse);

    uint32_t current_time = esp_timer_get_time() / 1000;
    uint32_t led_effect_duration = current_time - effect_start_time;

    ESP_LOGI(TAG, "Sequential fade out completed");
    ESP_LOGI(TAG, "Expected duration: %lu ms, Actual duration: %lu ms", expected_duration_ms, led_effect_duration);

    uint32_t difference = (led_effect_duration > expected_duration_ms)
                              ? (led_effect_duration - expected_duration_ms)
                              : (expected_duration_ms - led_effect_duration);

    if (difference > (expected_duration_ms / 20) && led_effect_running)
    {
        ESP_LOGW(TAG, "Effect timing was significantly off: %lu ms", difference);
    }

    // Mark task as complete
    led_effect_running = false;
    led_effect_task_handle = NULL;

    // Delete this task
    vTaskDelete(NULL);
}

/**
 * Time-based wrapper for the sequential fade-out animation
 * Calculates optimal parameters to achieve the desired animation duration
 *
 * @param start_led         First LED in the strip to animate
 * @param start_r           Starting red component (0-255)
 * @param start_g           Starting green component (0-255)
 * @param start_b           Starting blue component (0-255)
 * @param total_duration_ms Total time for the entire animation in milliseconds
 */
esp_err_t ada_led_strip_start_sequential_fade_out_with_duration(
    uint16_t start_led,
    uint8_t start_r,
    uint8_t start_g,
    uint8_t start_b,
    uint32_t total_duration_ms,
    bool reverse)
{
    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when starting sequential fade out");
        return ESP_ERR_TIMEOUT;
    }

    // Check if task is already running
    if (led_effect_task_handle != NULL || led_effect_running)
    {
        ESP_LOGW(TAG, "Sequential fade effect task already running. Stop it first.");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    int num_leds = CONFIG_ADA_LED_STRIP_MAX_LEDS;

    // Ensure reasonable minimum duration
    if (total_duration_ms < num_leds * 10)
    {
        total_duration_ms = num_leds * 10; // At least 10ms per LED
    }

    // Calculate timing parameters
    uint32_t led_start_sequence_time = (total_duration_ms * 80) / 100;
    uint32_t last_led_fade_time = total_duration_ms - led_start_sequence_time;

    // Calculate LED delay (time between starting each LED's fade)
    uint32_t led_delay_ms = led_start_sequence_time / (num_leds - 1);
    if (led_delay_ms < 5)
    {
        led_delay_ms = 5; // Minimum delay of 5ms
    }

    // Set fade_steps to match number of LEDs
    uint8_t fade_steps = num_leds;

    // Calculate step delay
    uint32_t step_delay_ms = last_led_fade_time / fade_steps;
    if (step_delay_ms < 5)
    {
        step_delay_ms = 5; // Minimum delay of 5ms
        // Recalculate last_led_fade_time to match new timing
        last_led_fade_time = step_delay_ms * fade_steps;
        // Optionally: Adjust led_start_sequence_time to maintain total duration
        led_start_sequence_time = total_duration_ms - last_led_fade_time;
        led_delay_ms = led_start_sequence_time / (num_leds - 1);
        if (led_delay_ms < 5)
        {
            led_delay_ms = 5;
        }
    }

    // Allocate and populate parameters for the task
    effect_params_t *params = malloc(sizeof(effect_params_t));
    if (params == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for fade task parameters");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_NO_MEM;
    }

    params->start_led = start_led;
    params->target_r = start_r; // Reusing target_r field for start_r
    params->target_g = start_g; // Reusing target_g field for start_g
    params->target_b = start_b; // Reusing target_b field for start_b
    params->fade_steps = fade_steps;
    params->step_delay_ms = step_delay_ms;
    params->led_delay_ms = led_delay_ms;
    params->reverse = reverse;
    params->expected_duration_ms = total_duration_ms; // Store expected duration

    // Set flag before creating task
    led_effect_running = true;

    // Release mutex before creating task
    xSemaphoreGive(led_strip_mutex);

    // Create the animation task
    BaseType_t task_created = xTaskCreate(
        ada_led_strip_sequential_fade_out_task,
        "ada_led_strip_sequential_fade_out_task",
        4096,
        params,
        5,
        &led_effect_task_handle);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create sequential fade out task");
        free(params);

        // Reset flags if task creation failed
        if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            led_effect_running = false;
            xSemaphoreGive(led_strip_mutex);
        }

        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ada_led_strip_color_breathing(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint8_t fade_steps,
    uint32_t step_delay_ms,
    uint8_t breath_cycles)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when setting color breathing");
        return ESP_ERR_TIMEOUT;
    }

    if (fade_steps == 0)
    {
        fade_steps = 1;
    }

    // Total phases in one complete breath cycle (fade in + fade out)
    uint16_t total_phases = fade_steps * 2;

    // Current phase in the breathing cycle
    uint16_t current_phase = 0;

    // Total number of phases across all cycles
    uint16_t total_phases_all_cycles = total_phases * breath_cycles;

    led_effect_running = true;

    // Continue until all breathing cycles are completed
    while (current_phase < total_phases_all_cycles && led_effect_running)
    {
        // Calculate the current position in the breathing cycle
        uint16_t cycle_phase = current_phase % total_phases;
        float brightness;

        // First half of the cycle: fade in
        if (cycle_phase < fade_steps)
        {
            brightness = (float)cycle_phase / fade_steps;
        }
        // Second half of the cycle: fade out
        else
        {
            brightness = (float)(total_phases - cycle_phase - 1) / fade_steps;
        }

        // Calculate the current RGB values
        uint8_t current_r = (uint8_t)(target_r * brightness);
        uint8_t current_g = (uint8_t)(target_g * brightness);
        uint8_t current_b = (uint8_t)(target_b * brightness);

        // Update all LEDs to the same brightness
        for (uint16_t i = 0; i < CONFIG_ADA_LED_STRIP_MAX_LEDS; i++)
        {
            uint16_t led_index = start_led + i;
            led_strip_set_pixel(led_strip, led_index, current_r, current_g, current_b);
        }

        // Refresh the LED strip to show the changes
        led_strip_refresh(led_strip);

        // Increment phase
        current_phase++;

        if (led_effect_running)
        {
            // Wait before the next update
            vTaskDelay(pdMS_TO_TICKS(step_delay_ms));
        }
    }

    // Final cleanup: set all LEDs to off
    clear_led_strip();

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return result;
}

static void ada_led_strip_color_breathing_task(void *arg)
{
    effect_params_t *params = (effect_params_t *)arg;

    // Local variables for clarity and to handle task cleanup
    uint16_t start_led = params->start_led;
    uint8_t target_r = params->target_r;
    uint8_t target_g = params->target_g;
    uint8_t target_b = params->target_b;
    uint8_t fade_steps = params->fade_steps;
    uint32_t step_delay_ms = params->step_delay_ms;
    uint8_t breath_cycles = params->breath_cycles;
    uint32_t expected_duration_ms = params->expected_duration_ms;

    // Free the parameters structure as we've copied the values
    free(params);

    uint32_t effect_start_time = esp_timer_get_time() / 1000;

    ada_led_strip_color_breathing(
        start_led,
        target_r,
        target_g,
        target_b,
        fade_steps,
        step_delay_ms,
        breath_cycles);

    uint32_t current_time = esp_timer_get_time() / 1000;
    uint32_t led_effect_duration = current_time - effect_start_time;

    ESP_LOGI(TAG, "Color breathing effect completed");
    ESP_LOGI(TAG, "Expected duration: %lu ms, Actual duration: %lu ms", expected_duration_ms, led_effect_duration);

    uint32_t difference = (led_effect_duration > expected_duration_ms)
                              ? (led_effect_duration - expected_duration_ms)
                              : (expected_duration_ms - led_effect_duration);

    if (difference > (expected_duration_ms / 20) && led_effect_running)
    {
        ESP_LOGW(TAG, "Effect timing was significantly off: %lu ms", difference);
    }

    // Mark task as complete
    led_effect_running = false;
    led_effect_task_handle = NULL;

    // Delete this task
    vTaskDelete(NULL);
}

/**
 * Time-based wrapper for the synchronous color breathing animation
 * Calculates optimal parameters to achieve the desired animation duration
 * All LEDs breathe in unison with the same pattern
 *
 * @param start_led         First LED in the strip to animate
 * @param target_r          Target red component (0-255)
 * @param target_g          Target green component (0-255)
 * @param target_b          Target blue component (0-255)
 * @param total_duration_ms Total time for the entire animation in milliseconds
 * @param breath_cycles     Number of complete breath cycles
 * @param num_leds          Number of LEDs to control
 */
esp_err_t ada_led_strip_start_color_breathing_with_duration(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint32_t total_duration_ms,
    uint8_t breath_cycles)
{
    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when starting color breathing");
        return ESP_ERR_TIMEOUT;
    }

    // Check if task is already running
    if (led_effect_task_handle != NULL || led_effect_running)
    {
        ESP_LOGW(TAG, "Effect already running. Stop it first.");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Apply a reasonable minimum duration
    uint32_t min_duration_ms = breath_cycles * 200; // At least 200ms per cycle
    if (total_duration_ms < min_duration_ms)
    {
        total_duration_ms = min_duration_ms;
    }

    // Use a fixed number of steps for smooth breathing
    uint8_t fade_steps = 30; // Fixed number of steps for smooth fade

    // Calculate total steps for a complete breath cycle (fade in + fade out)
    uint16_t total_phases = fade_steps * 2;

    // Calculate total phases for all cycles
    uint16_t total_phases_all_cycles = total_phases * breath_cycles;

    // Calculate the step delay (time between brightness updates)
    uint32_t step_delay_ms = total_duration_ms / total_phases_all_cycles;
    if (step_delay_ms < 5)
    {
        step_delay_ms = 5; // Minimum delay of 5ms
    }

    // Calculate actual expected duration based on our parameters
    uint32_t calculated_duration = step_delay_ms * total_phases_all_cycles;

    ESP_LOGI(TAG, "Color breathing: requested=%lums, calculated=%lums, cycles=%u, step_delay=%lums",
             total_duration_ms, calculated_duration, breath_cycles, step_delay_ms);

    // Allocate and populate parameters for the task
    effect_params_t *params = malloc(sizeof(effect_params_t));
    if (params == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for breathing task parameters");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_NO_MEM;
    }

    params->start_led = start_led;
    params->target_r = target_r;
    params->target_g = target_g;
    params->target_b = target_b;
    params->fade_steps = fade_steps;
    params->step_delay_ms = step_delay_ms;
    params->breath_cycles = breath_cycles;
    params->expected_duration_ms = calculated_duration;

    // Set flag before creating task
    led_effect_running = true;

    // Release mutex before creating task
    xSemaphoreGive(led_strip_mutex);

    // Create the animation task
    BaseType_t task_created = xTaskCreate(
        ada_led_strip_color_breathing_task,
        "ada_led_strip_color_breathing_task",
        4096,
        params,
        5,
        &led_effect_task_handle);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create color breathing task");
        free(params);

        // Reset flags if task creation failed
        if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            led_effect_running = false;
            xSemaphoreGive(led_strip_mutex);
        }

        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * Stop any currently running LED strip effect
 *
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t ada_led_strip_stop_effect(void)
{
    // Set the flag to indicate effect should stop
    if (!led_effect_running)
    {
        ESP_LOGW(TAG, "There is no running LED effect");
        return ESP_ERR_INVALID_STATE;
    }

    // Signal the running task to stop
    led_effect_running = false;

    // Wait for a short time to give the task a chance to exit cleanly
    vTaskDelay(pdMS_TO_TICKS(50));

    // Try to take the mutex
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Could not acquire mutex after stopping effect");
        return ESP_ERR_TIMEOUT;
    }

    // If the task is still running after the delay, we need to delete it forcefully
    if (led_effect_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Forcefully deleting effect task that didn't stop cleanly");
        vTaskDelete(led_effect_task_handle);
        led_effect_task_handle = NULL;
    }

    ESP_LOGI(TAG, "LED effect stopped successfully");

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return ESP_OK;
}

esp_err_t start_rainbow_effect(void)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when starting rainbow effect");
        return ESP_ERR_TIMEOUT;
    }

    // Check if task is already running
    if (led_effect_task_handle != NULL || led_effect_running)
    {
        ESP_LOGW(TAG, "Rainbow effect task already running. Stop it first.");
        xSemaphoreGive(led_strip_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting rainbow effect on LED strip");

    // Clear the strip before starting animation
    result = led_strip_clear(led_strip);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(result));
        xSemaphoreGive(led_strip_mutex);
        return result;
    }

    // Set flag before creating task
    led_effect_running = true;

    // Release mutex before creating task
    xSemaphoreGive(led_strip_mutex);

    // Create the animation task
    BaseType_t task_created = xTaskCreate(
        rainbow_effect_Task,
        "rainbow_effect_Task",
        4096,
        NULL,
        5,
        &led_effect_task_handle);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create rainbow effect task");

        // Reset flags if task creation failed
        if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            led_effect_running = false;
            xSemaphoreGive(led_strip_mutex);
        }

        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t set_all_leds_to_color(
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when setting all LEDs to white");
        return ESP_ERR_TIMEOUT;
    }

    // Set all LEDs to white
    for (int i = 0; i < CONFIG_ADA_LED_STRIP_MAX_LEDS; i++)
    {
        led_strip_set_pixel(led_strip, i, r, g, b); // Set to white (RGB: 255, 255, 255)
    }

    // Refresh the strip to send data
    result = led_strip_refresh(led_strip);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(result));
    }

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return result;
}

esp_err_t clear_led_strip(void)
{
    esp_err_t result = ESP_OK;

    // Try to take the mutex with timeout
    if (xSemaphoreTake(led_strip_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex when clearing LED strip");
        return ESP_ERR_TIMEOUT;
    }

    // Clear the LED strip
    result = led_strip_clear(led_strip);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "LED strip cleared successfully");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(result));
    }

    // Release mutex
    xSemaphoreGive(led_strip_mutex);

    return result;
}

void led_test(void)
{
    while (1)
    {
        ada_led_strip_start_sequential_fade_in_with_duration(0, 0, 0, 100, 2000, false);

        vTaskDelay(pdMS_TO_TICKS(2100));

        ada_led_strip_start_sequential_fade_out_with_duration(CONFIG_ADA_LED_STRIP_MAX_LEDS - 1, 0, 0, 100, 2000, true);

        vTaskDelay(pdMS_TO_TICKS(1000));

        ada_led_strip_stop_effect();
        clear_led_strip();

        set_all_leds_to_color(100, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));

        clear_led_strip();
        ada_led_strip_start_sequential_fade_in_with_duration(0, 0, 100, 0, 5000, false);
        vTaskDelay(pdMS_TO_TICKS(3000));

        ada_led_strip_stop_effect();

        clear_led_strip();
    }
}
