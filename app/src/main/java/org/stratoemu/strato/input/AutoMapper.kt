/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.input

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Creates a complete mapping for a standard Android game controller without
 * requiring the user to press every button individually.
 */
object AutoMapper {
    /**
     * Maps [device] to [controller] using Android's standard gamepad button
     * key codes and joystick axes.
     *
     * @return The number of inputs that were mapped.
     */
    fun map(inputManager : InputManager, controller : Controller, device : InputDevice) : Int {
        val descriptor = device.descriptor

        // Remove the previous mapping for this physical controller so that
        // stale mappings do not survive when the user selects another pad.
        inputManager.eventMap.keys.filter { it?.descriptor == descriptor }.forEach {
            inputManager.eventMap.remove(it)
        }

        var mapped = 0

        fun mapButton(keyCode : Int, button : ButtonId) {
            if (!supportsButton(controller, button))
                return

            inputManager.eventMap.filterValues { it is ButtonGuestEvent && it == ButtonGuestEvent(controller.id, button) }
                .keys.forEach { inputManager.eventMap.remove(it) }

            inputManager.eventMap[KeyHostEvent(descriptor, keyCode)] = ButtonGuestEvent(controller.id, button)
            mapped++
        }

        fun mapAxis(axis : Int, polarity : Boolean, guestAxis : AxisId) {
            if (!supportsAxis(controller, guestAxis))
                return

            inputManager.eventMap.filterValues { it is AxisGuestEvent && it == AxisGuestEvent(controller.id, guestAxis, polarity) }
                .keys.forEach { inputManager.eventMap.remove(it) }

            inputManager.eventMap[MotionHostEvent(descriptor, axis, polarity)] = AxisGuestEvent(controller.id, guestAxis, polarity)
            mapped++
        }

        val buttons = mapOf(
            KeyEvent.KEYCODE_BUTTON_A to ButtonId.A,
            KeyEvent.KEYCODE_BUTTON_B to ButtonId.B,
            KeyEvent.KEYCODE_BUTTON_X to ButtonId.X,
            KeyEvent.KEYCODE_BUTTON_Y to ButtonId.Y,
            KeyEvent.KEYCODE_BUTTON_L1 to ButtonId.L,
            KeyEvent.KEYCODE_BUTTON_R1 to ButtonId.R,
            KeyEvent.KEYCODE_BUTTON_L2 to ButtonId.ZL,
            KeyEvent.KEYCODE_BUTTON_R2 to ButtonId.ZR,
            KeyEvent.KEYCODE_BUTTON_START to ButtonId.Plus,
            KeyEvent.KEYCODE_BUTTON_SELECT to ButtonId.Minus,
            KeyEvent.KEYCODE_DPAD_UP to ButtonId.DpadUp,
            KeyEvent.KEYCODE_DPAD_DOWN to ButtonId.DpadDown,
            KeyEvent.KEYCODE_DPAD_LEFT to ButtonId.DpadLeft,
            KeyEvent.KEYCODE_DPAD_RIGHT to ButtonId.DpadRight,
            KeyEvent.KEYCODE_BUTTON_THUMBL to ButtonId.LeftStick,
            KeyEvent.KEYCODE_BUTTON_THUMBR to ButtonId.RightStick,
            KeyEvent.KEYCODE_BUTTON_MODE to ButtonId.Menu
        )

        buttons.forEach { (keyCode, button) -> mapButton(keyCode, button) }

        val hasAxis = { axis : Int -> device.getMotionRange(axis, device.sources) != null }

        if (hasAxis(MotionEvent.AXIS_X)) {
            mapAxis(MotionEvent.AXIS_X, true, AxisId.LX)
            mapAxis(MotionEvent.AXIS_X, false, AxisId.LX)
        }

        if (hasAxis(MotionEvent.AXIS_Y)) {
            // InputHandler inverts Y before sending it to the guest, so the
            // Android negative pole is the guest's Up direction.
            mapAxis(MotionEvent.AXIS_Y, false, AxisId.LY)
            mapAxis(MotionEvent.AXIS_Y, true, AxisId.LY)
        }

        val rightX = when {
            hasAxis(MotionEvent.AXIS_RX) -> MotionEvent.AXIS_RX
            hasAxis(MotionEvent.AXIS_Z) -> MotionEvent.AXIS_Z
            else -> null
        }
        val rightY = when {
            hasAxis(MotionEvent.AXIS_RY) -> MotionEvent.AXIS_RY
            hasAxis(MotionEvent.AXIS_RZ) -> MotionEvent.AXIS_RZ
            else -> null
        }

        rightX?.let {
            mapAxis(it, true, AxisId.RX)
            mapAxis(it, false, AxisId.RX)
        }
        rightY?.let {
            mapAxis(it, false, AxisId.RY)
            mapAxis(it, true, AxisId.RY)
        }

        // Modern Android gamepads expose the triggers independently. Some
        // older devices expose them as Z/RZ when those axes are not used by
        // the right stick.
        if (hasAxis(MotionEvent.AXIS_LTRIGGER) && mapTrigger(MotionEvent.AXIS_LTRIGGER, ButtonId.ZL, inputManager, controller, descriptor))
            mapped++
        if (hasAxis(MotionEvent.AXIS_RTRIGGER) && mapTrigger(MotionEvent.AXIS_RTRIGGER, ButtonId.ZR, inputManager, controller, descriptor))
            mapped++

        if (!hasAxis(MotionEvent.AXIS_LTRIGGER) && rightX != MotionEvent.AXIS_Z && hasAxis(MotionEvent.AXIS_Z) &&
            mapTrigger(MotionEvent.AXIS_Z, ButtonId.ZL, inputManager, controller, descriptor))
            mapped++
        if (!hasAxis(MotionEvent.AXIS_RTRIGGER) && rightY != MotionEvent.AXIS_RZ && hasAxis(MotionEvent.AXIS_RZ) &&
            mapTrigger(MotionEvent.AXIS_RZ, ButtonId.ZR, inputManager, controller, descriptor))
            mapped++

        inputManager.syncFile()
        return mapped
    }

    private fun mapTrigger(
        axis : Int,
        button : ButtonId,
        inputManager : InputManager,
        controller : Controller,
        descriptor : String
    ) : Boolean {
        if (!supportsButton(controller, button))
            return false

        inputManager.eventMap.filterValues { it is ButtonGuestEvent && it == ButtonGuestEvent(controller.id, button) }
            .keys.forEach { inputManager.eventMap.remove(it) }

        inputManager.eventMap[MotionHostEvent(descriptor, axis, true)] = ButtonGuestEvent(controller.id, button, 0.5f)
        return true
    }

    private fun supportsButton(controller : Controller, button : ButtonId) : Boolean {
        return button == ButtonId.Menu || controller.type.buttons.contains(button) || controller.type.sticks.any { it.button == button }
    }

    private fun supportsAxis(controller : Controller, axis : AxisId) : Boolean {
        return controller.type.sticks.any { it.xAxis == axis || it.yAxis == axis }
    }
}
