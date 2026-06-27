/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.input

import android.content.Context
import android.hardware.input.InputManager.InputDeviceListener
import android.util.Log
import android.view.InputDevice
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.R
import java.io.*
import javax.inject.Inject
import javax.inject.Singleton

/**
 * This object is used to manage all transactions with storing/retrieving data in relation to input.
 *
 * Extended with plug-and-play auto-profile support:
 * - When a physical gamepad connects, [onDeviceAdded] is called automatically.
 * - The device name is matched against [ControllerProfiles] and a full mapping is
 *   applied immediately — no manual binding required.
 * - If the device already has a mapping (user customised it), it is left untouched.
 */
@Singleton
class InputManager @Inject constructor(@ApplicationContext private val context : Context)
    : InputDeviceListener {

    private val file = File("${context.applicationInfo.dataDir}/input.bin")

    lateinit var controllers : HashMap<Int, Controller>
    lateinit var eventMap : HashMap<HostEvent?, GuestEvent?>

    /** Android InputManager for device-connection callbacks */
    private val androidInputManager =
        context.getSystemService(Context.INPUT_SERVICE) as android.hardware.input.InputManager

    init {
        run {
            try {
                if (file.exists() && file.length() != 0L) {
                    syncObjects()
                    return@run
                }
            } catch (e : Exception) {
                Log.e(TAG, e.localizedMessage ?: "InputManager cannot read \"${file.absolutePath}\"")
            }

            controllers = hashMapOf(
                0 to Controller(0, ControllerType.HandheldProController,
                                Controller.BuiltinRumbleDeviceDescriptor,
                                context.getString(R.string.builtin_vibrator)),
                1 to Controller(1, ControllerType.None),
                2 to Controller(2, ControllerType.None),
                3 to Controller(3, ControllerType.None),
                4 to Controller(4, ControllerType.None),
                5 to Controller(5, ControllerType.None),
                6 to Controller(6, ControllerType.None),
                7 to Controller(7, ControllerType.None),
            )
            eventMap = hashMapOf()
            syncFile()
        }

        // Register for device connect/disconnect events
        androidInputManager.registerInputDeviceListener(this, null)

        // Auto-profile any gamepad that is already connected at startup
        autoProfileAllConnectedDevices()
    }

    // ─── Plug-and-play auto-profiling ─────────────────────────────────────────

    /**
     * Walks every currently connected Android input device and applies a profile
     * to the first free controller slot if the device is a gamepad and has no
     * existing mapping.
     */
    private fun autoProfileAllConnectedDevices() {
        InputDevice.getDeviceIds()
            .mapNotNull { InputDevice.getDevice(it) }
            .filter { isGamepad(it) }
            .forEach { applyProfileIfNeeded(it) }
    }

    /**
     * Called by Android when a new input device is connected (USB plug or BT pair).
     * Immediately applies the best-matching profile for that device.
     */
    override fun onInputDeviceAdded(deviceId : Int) {
        val device = InputDevice.getDevice(deviceId) ?: return
        if (!isGamepad(device)) return

        Log.i(TAG, "Gamepad connected: \"${device.name}\" (descriptor=${device.descriptor})")
        applyProfileIfNeeded(device)
    }

    override fun onInputDeviceRemoved(deviceId : Int) {
        Log.i(TAG, "Input device $deviceId disconnected")
    }

    override fun onInputDeviceChanged(deviceId : Int) { /* no-op */ }

    /**
     * Returns true if [device] is a gamepad (has gamepad source and at least one button).
     */
    private fun isGamepad(device : InputDevice) : Boolean =
        device.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
        device.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK

    /**
     * Applies a [ControllerProfile] for [device] to the first free controller slot.
     *
     * - If this device's descriptor is already mapped in [eventMap], we leave it alone
     *   (user may have customised it).
     * - If all 8 slots are occupied, we do nothing.
     */
    private fun applyProfileIfNeeded(device : InputDevice) {
        val descriptor = device.descriptor

        // Check if this descriptor already has at least one mapping → user configured it
        val alreadyMapped = eventMap.keys.any { it?.descriptor == descriptor }
        if (alreadyMapped) {
            Log.d(TAG, "Device \"${device.name}\" already mapped — skipping auto-profile")
            return
        }

        // Find a free controller slot (slot 0 is always the handheld/pro controller)
        val freeSlot = controllers.entries
            .sortedBy { it.key }
            .firstOrNull { it.value.type == ControllerType.None }
            ?.key

        if (freeSlot == null) {
            Log.w(TAG, "No free controller slot for \"${device.name}\"")
            return
        }

        val profile = ControllerProfiles.findProfile(device.name)
        Log.i(TAG, "Auto-applying profile for \"${device.name}\" → slot $freeSlot " +
                   "(rumble=${profile.rumbleSupported})")

        // Upgrade slot to ProController
        controllers[freeSlot] = ProController(freeSlot).apply {
            if (profile.rumbleSupported) {
                rumbleDeviceDescriptor = descriptor
                rumbleDeviceName = device.name
            }
        }

        // Write all button + axis mappings
        ControllerProfiles.buildEventMap(profile, descriptor, freeSlot, eventMap)

        syncFile()
    }

    // ─── Existing API ─────────────────────────────────────────────────────────

    fun syncObjects() {
        ObjectInputStream(FileInputStream(file)).use {
            @Suppress("UNCHECKED_CAST")
            controllers = it.readObject() as HashMap<Int, Controller>

            @Suppress("UNCHECKED_CAST")
            eventMap = it.readObject() as HashMap<HostEvent?, GuestEvent?>
        }
    }

    fun syncFile() {
        for (controller in controllers.values) {
            for (button in ButtonId.values()) {
                if (button != ButtonId.Menu &&
                    !(controller.type.buttons.contains(button) ||
                      controller.type.sticks.any { it.button == button })) {
                    val guestEvent = ButtonGuestEvent(controller.id, button)
                    eventMap.filterValues { it is ButtonGuestEvent && it == guestEvent }
                        .keys.forEach { eventMap.remove(it) }
                }
            }

            for (stick in StickId.values()) {
                if (!controller.type.sticks.contains(stick)) {
                    for (axis in arrayOf(stick.xAxis, stick.yAxis)) {
                        for (polarity in booleanArrayOf(true, false)) {
                            val guestEvent = AxisGuestEvent(controller.id, axis, polarity)
                            eventMap.filterValues { it is AxisGuestEvent && it == guestEvent }
                                .keys.forEach { eventMap.remove(it) }
                        }
                    }
                }
            }
        }

        ObjectOutputStream(FileOutputStream(file)).use {
            it.writeObject(controllers)
            it.writeObject(eventMap)
            it.flush()
        }
    }

    companion object {
        private const val TAG = "InputManager"
    }
}
