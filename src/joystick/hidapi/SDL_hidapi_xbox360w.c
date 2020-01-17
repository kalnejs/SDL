/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "SDL_hints.h"
#include "SDL_log.h"
#include "SDL_events.h"
#include "SDL_timer.h"
#include "SDL_joystick.h"
#include "SDL_gamecontroller.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"


#ifdef SDL_JOYSTICK_HIDAPI_XBOX360

#define USB_PACKET_LENGTH   64


typedef struct {
    SDL_bool connected;
    Uint8 last_state[USB_PACKET_LENGTH];
    Uint32 rumble_expiration;
} SDL_DriverXbox360W_Context;


static SDL_bool
HIDAPI_DriverXbox360W_IsSupportedDevice(Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, const char *name)
{
    const Uint16 MICROSOFT_USB_VID = 0x045e;

    if (vendor_id == MICROSOFT_USB_VID) {
        return (product_id == 0x0291 || product_id == 0x0719);
    }
    return SDL_FALSE;
}

static const char *
HIDAPI_DriverXbox360W_GetDeviceName(Uint16 vendor_id, Uint16 product_id)
{
    return "Xbox 360 Wireless Controller";
}

static SDL_bool SetSlotLED(hid_device *dev, Uint8 slot)
{
    Uint8 mode = 0x02 + slot;
    const Uint8 led_packet[] = { 0x00, 0x00, 0x08, (0x40 + (mode % 0x0e)), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    if (hid_write(dev, led_packet, sizeof(led_packet)) != sizeof(led_packet)) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

static void
UpdatePowerLevel(SDL_Joystick *joystick, Uint8 level)
{
    float normalized_level = (float)level / 255.0f;

    if (normalized_level <= 0.05f) {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_EMPTY;
    } else if (normalized_level <= 0.20f) {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_LOW;
    } else if (normalized_level <= 0.70f) {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_MEDIUM;
    } else {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_FULL;
    }
}

static SDL_bool
HIDAPI_DriverXbox360W_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360W_Context *ctx;

    /* Requests controller presence information from the wireless dongle */
    const Uint8 init_packet[] = { 0x08, 0x00, 0x0F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    ctx = (SDL_DriverXbox360W_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        SDL_OutOfMemory();
        return SDL_FALSE;
    }

    device->dev = hid_open_path(device->path, 0);
    if (!device->dev) {
        SDL_free(ctx);
        SDL_SetError("Couldn't open %s", device->path);
        return SDL_FALSE;
    }
    device->context = ctx;

    if (hid_write(device->dev, init_packet, sizeof(init_packet)) != sizeof(init_packet)) {
        SDL_SetError("Couldn't write init packet");
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static int
HIDAPI_DriverXbox360W_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void
HIDAPI_DriverXbox360W_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SetSlotLED(device->dev, (player_index % 4));
}

static SDL_bool
HIDAPI_DriverXbox360W_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    SDL_zeroa(ctx->last_state);

    /* Initialize the joystick capabilities */
    joystick->nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    joystick->naxes = SDL_CONTROLLER_AXIS_MAX;
    joystick->epowerlevel = SDL_JOYSTICK_POWER_UNKNOWN;

    return SDL_TRUE;
}

static int
HIDAPI_DriverXbox360W_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    Uint8 rumble_packet[] = { 0x00, 0x01, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    rumble_packet[5] = (low_frequency_rumble >> 8);
    rumble_packet[6] = (high_frequency_rumble >> 8);

    if (hid_write(device->dev, rumble_packet, sizeof(rumble_packet)) != sizeof(rumble_packet)) {
        return SDL_SetError("Couldn't send rumble packet");
    }

    if ((low_frequency_rumble || high_frequency_rumble) && duration_ms) {
        ctx->rumble_expiration = SDL_GetTicks() + SDL_min(duration_ms, SDL_MAX_RUMBLE_DURATION_MS);
        if (!ctx->rumble_expiration) {
            ctx->rumble_expiration = 1;
        }
    } else {
        ctx->rumble_expiration = 0;
    }
    return 0;
}

static void
HIDAPI_DriverXbox360W_HandleStatePacket(SDL_Joystick *joystick, hid_device *dev, SDL_DriverXbox360W_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    const SDL_bool invert_y_axes = SDL_TRUE;

    if (ctx->last_state[2] != data[2]) {
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_UP, (data[2] & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, (data[2] & 0x02) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, (data[2] & 0x04) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, (data[2] & 0x08) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_START, (data[2] & 0x10) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_BACK, (data[2] & 0x20) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSTICK, (data[2] & 0x40) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSTICK, (data[2] & 0x80) ? SDL_PRESSED : SDL_RELEASED);
    }

    if (ctx->last_state[3] != data[3]) {
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, (data[3] & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, (data[3] & 0x02) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_GUIDE, (data[3] & 0x04) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_A, (data[3] & 0x10) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_B, (data[3] & 0x20) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_X, (data[3] & 0x40) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_Y, (data[3] & 0x80) ? SDL_PRESSED : SDL_RELEASED);
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, axis);
    axis = *(Sint16*)(&data[6]);
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, axis);
    axis = *(Sint16*)(&data[8]);
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, axis);
    axis = *(Sint16*)(&data[10]);
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, axis);
    axis = *(Sint16*)(&data[12]);
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, axis);

    SDL_memcpy(ctx->last_state, data, SDL_min(size, sizeof(ctx->last_state)));
}

static SDL_bool
HIDAPI_DriverXbox360W_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_JoystickFromInstanceID(device->joysticks[0]);
    }

    while ((size = hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (size == 2 && data[0] == 0x08) {
            SDL_bool connected = (data[1] & 0x80) ? SDL_TRUE : SDL_FALSE;
#ifdef DEBUG_JOYSTICK
            SDL_Log("Connected = %s\n", connected ? "TRUE" : "FALSE");
#endif
            if (connected != ctx->connected) {
                ctx->connected = connected;

                if (connected) {
                    SDL_JoystickID joystickID;

                    HIDAPI_JoystickConnected(device, &joystickID);

                } else if (device->num_joysticks > 0) {
                    HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
                }
            }
        } else if (size == 29 && data[0] == 0x00 && data[1] == 0x0f && data[2] == 0x00 && data[3] == 0xf0) {
            /* Serial number is data[7-13] */
#ifdef DEBUG_JOYSTICK
            SDL_Log("Battery status (initial): %d\n", data[17]);
#endif
            if (joystick) {
                UpdatePowerLevel(joystick, data[17]);
            }
        } else if (size == 29 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x13) {
#ifdef DEBUG_JOYSTICK
            SDL_Log("Battery status: %d\n", data[4]);
#endif
            if (joystick) {
                UpdatePowerLevel(joystick, data[4]);
            }
        } else if (size == 29 && data[0] == 0x00 && (data[1] & 0x01) == 0x01) {
            if (joystick) {
                HIDAPI_DriverXbox360W_HandleStatePacket(joystick, device->dev, ctx, data+4, size-4);
            }
        }
    }

    if (joystick) {
        if (ctx->rumble_expiration) {
            Uint32 now = SDL_GetTicks();
            if (SDL_TICKS_PASSED(now, ctx->rumble_expiration)) {
                HIDAPI_DriverXbox360W_RumbleJoystick(device, joystick, 0, 0, 0);
            }
        }

        if (size < 0) {
            /* Read error, device is disconnected */
            HIDAPI_JoystickDisconnected(device, joystick->instance_id);
        }
    }
    return (size >= 0);
}

static void
HIDAPI_DriverXbox360W_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void
HIDAPI_DriverXbox360W_FreeDevice(SDL_HIDAPI_Device *device)
{
    hid_close(device->dev);
    device->dev = NULL;

    SDL_free(device->context);
    device->context = NULL;
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverXbox360W =
{
    SDL_HINT_JOYSTICK_HIDAPI_XBOX,
    SDL_TRUE,
    HIDAPI_DriverXbox360W_IsSupportedDevice,
    HIDAPI_DriverXbox360W_GetDeviceName,
    HIDAPI_DriverXbox360W_InitDevice,
    HIDAPI_DriverXbox360W_GetDevicePlayerIndex,
    HIDAPI_DriverXbox360W_SetDevicePlayerIndex,
    HIDAPI_DriverXbox360W_UpdateDevice,
    HIDAPI_DriverXbox360W_OpenJoystick,
    HIDAPI_DriverXbox360W_RumbleJoystick,
    HIDAPI_DriverXbox360W_CloseJoystick,
    HIDAPI_DriverXbox360W_FreeDevice
};

#endif /* SDL_JOYSTICK_HIDAPI_XBOX360 */

#endif /* SDL_JOYSTICK_HIDAPI */

/* vi: set ts=4 sw=4 expandtab: */
