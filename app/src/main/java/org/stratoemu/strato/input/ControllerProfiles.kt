/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2024 Strato Team and Contributors
 *
 * ControllerProfiles — plug-and-play mappings for the most common gamepads.
 *
 * When a physical device connects whose name matches one of the entries below,
 * its full button + axis mapping is applied immediately with no user interaction.
 *
 * Key codes reference: android.view.KeyEvent.KEYCODE_BUTTON_*
 * Axis codes reference: android.view.MotionEvent.AXIS_*
 */

package org.stratoemu.strato.input

import android.view.KeyEvent
import android.view.MotionEvent

/**
 * A complete pre-built mapping for one physical controller model.
 *
 * @param namePatterns  Substrings matched (case-insensitive) against [InputDevice.name].
 *                      First match wins. Keep patterns specific enough to avoid false positives.
 * @param buttons       Map of Android KeyEvent keyCode → Switch [ButtonId]
 * @param axes          Map of (MotionEvent axis, polarity) → Switch [AxisId] + guest polarity
 * @param rumbleSupported  Whether this controller family is known to support rumble
 */
data class ControllerProfile(
    val namePatterns : List<String>,
    val buttons : Map<Int, ButtonId>,
    val axes : Map<Pair<Int, Boolean>, Pair<AxisId, Boolean>>,
    val rumbleSupported : Boolean = true
)

object ControllerProfiles {

    // ─────────────────────────────────────────────────────────────────────────
    // Standard Android gamepad button → Switch button
    // These codes are the same across virtually all Bluetooth/USB gamepads that
    // follow the Android gamepad convention (HID → Android keycode mapping).
    // ─────────────────────────────────────────────────────────────────────────

    private val standardButtons = mapOf(
        KeyEvent.KEYCODE_BUTTON_A     to ButtonId.B,       // Android A = Switch B (cross-layout)
        KeyEvent.KEYCODE_BUTTON_B     to ButtonId.A,       // Android B = Switch A
        KeyEvent.KEYCODE_BUTTON_X     to ButtonId.Y,       // Android X = Switch Y
        KeyEvent.KEYCODE_BUTTON_Y     to ButtonId.X,       // Android Y = Switch X
        KeyEvent.KEYCODE_BUTTON_L1    to ButtonId.L,
        KeyEvent.KEYCODE_BUTTON_R1    to ButtonId.R,
        KeyEvent.KEYCODE_BUTTON_L2    to ButtonId.ZL,
        KeyEvent.KEYCODE_BUTTON_R2    to ButtonId.ZR,
        KeyEvent.KEYCODE_BUTTON_THUMBL to ButtonId.LeftStick,
        KeyEvent.KEYCODE_BUTTON_THUMBR to ButtonId.RightStick,
        KeyEvent.KEYCODE_BUTTON_START to ButtonId.Plus,
        KeyEvent.KEYCODE_BUTTON_SELECT to ButtonId.Minus,
        KeyEvent.KEYCODE_DPAD_UP      to ButtonId.DpadUp,
        KeyEvent.KEYCODE_DPAD_DOWN    to ButtonId.DpadDown,
        KeyEvent.KEYCODE_DPAD_LEFT    to ButtonId.DpadLeft,
        KeyEvent.KEYCODE_DPAD_RIGHT   to ButtonId.DpadRight,
        KeyEvent.KEYCODE_BUTTON_MODE  to ButtonId.Menu,
    )

    /** Standard axes — same for almost every gamepad following Android conventions */
    private val standardAxes = mapOf(
        Pair(MotionEvent.AXIS_X,  true)  to Pair(AxisId.LX,  true),
        Pair(MotionEvent.AXIS_X,  false) to Pair(AxisId.LX,  false),
        Pair(MotionEvent.AXIS_Y,  false) to Pair(AxisId.LY,  false),
        Pair(MotionEvent.AXIS_Y,  true)  to Pair(AxisId.LY,  true),
        Pair(MotionEvent.AXIS_Z,  true)  to Pair(AxisId.RX,  true),
        Pair(MotionEvent.AXIS_Z,  false) to Pair(AxisId.RX,  false),
        Pair(MotionEvent.AXIS_RZ, false) to Pair(AxisId.RY,  false),
        Pair(MotionEvent.AXIS_RZ, true)  to Pair(AxisId.RY,  true),
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Xbox controllers
    // Covers: Xbox One S/X, Xbox Series S/X, Xbox Elite 1/2
    // Both Bluetooth and USB connections use the same keycodes on Android 8+
    // ─────────────────────────────────────────────────────────────────────────

    private val xboxProfile = ControllerProfile(
        namePatterns = listOf("xbox", "x-box", "microsoft controller"),
        buttons = standardButtons + mapOf(
            KeyEvent.KEYCODE_BUTTON_START  to ButtonId.Plus,
            KeyEvent.KEYCODE_BUTTON_SELECT to ButtonId.Minus,
            KeyEvent.KEYCODE_BUTTON_MODE   to ButtonId.Menu,  // Xbox button
        ),
        axes = standardAxes + mapOf(
            // Xbox triggers report on AXIS_LTRIGGER / AXIS_RTRIGGER (positive only)
            Pair(MotionEvent.AXIS_LTRIGGER, true) to Pair(AxisId.LX, true),  // unused — ZL via L2
            Pair(MotionEvent.AXIS_RTRIGGER, true) to Pair(AxisId.RX, true),  // unused — ZR via R2
        ),
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // PlayStation controllers
    // DualShock 4 (DS4), DualSense (PS5)
    // ─────────────────────────────────────────────────────────────────────────

    private val playstationProfile = ControllerProfile(
        namePatterns = listOf(
            "dualshock", "dualsense", "dual shock", "dual sense",
            "wireless controller",   // DualSense reports this name via Bluetooth
            "ps4 controller", "ps5 controller",
            "sony computer entertainment"
        ),
        // PS layout: Cross=A, Circle=B, Square=X, Triangle=Y on Android
        // Switch layout: B=bottom, A=right, Y=left, X=top → swap accordingly
        buttons = mapOf(
            KeyEvent.KEYCODE_BUTTON_A      to ButtonId.B,    // Cross   → B
            KeyEvent.KEYCODE_BUTTON_B      to ButtonId.A,    // Circle  → A
            KeyEvent.KEYCODE_BUTTON_X      to ButtonId.Y,    // Square  → Y
            KeyEvent.KEYCODE_BUTTON_Y      to ButtonId.X,    // Triangle→ X
            KeyEvent.KEYCODE_BUTTON_L1     to ButtonId.L,
            KeyEvent.KEYCODE_BUTTON_R1     to ButtonId.R,
            KeyEvent.KEYCODE_BUTTON_L2     to ButtonId.ZL,
            KeyEvent.KEYCODE_BUTTON_R2     to ButtonId.ZR,
            KeyEvent.KEYCODE_BUTTON_THUMBL to ButtonId.LeftStick,
            KeyEvent.KEYCODE_BUTTON_THUMBR to ButtonId.RightStick,
            KeyEvent.KEYCODE_BUTTON_START  to ButtonId.Plus,   // Options → +
            KeyEvent.KEYCODE_BUTTON_SELECT to ButtonId.Minus,  // Create/Share → –
            KeyEvent.KEYCODE_BUTTON_MODE   to ButtonId.Menu,   // PS button
            KeyEvent.KEYCODE_DPAD_UP       to ButtonId.DpadUp,
            KeyEvent.KEYCODE_DPAD_DOWN     to ButtonId.DpadDown,
            KeyEvent.KEYCODE_DPAD_LEFT     to ButtonId.DpadLeft,
            KeyEvent.KEYCODE_DPAD_RIGHT    to ButtonId.DpadRight,
        ),
        // DualShock 4 / DualSense: right stick on AXIS_RX / AXIS_RY
        axes = mapOf(
            Pair(MotionEvent.AXIS_X,   true)  to Pair(AxisId.LX, true),
            Pair(MotionEvent.AXIS_X,   false) to Pair(AxisId.LX, false),
            Pair(MotionEvent.AXIS_Y,   false) to Pair(AxisId.LY, false),
            Pair(MotionEvent.AXIS_Y,   true)  to Pair(AxisId.LY, true),
            Pair(MotionEvent.AXIS_Z,   true)  to Pair(AxisId.RX, true),
            Pair(MotionEvent.AXIS_Z,   false) to Pair(AxisId.RX, false),
            Pair(MotionEvent.AXIS_RZ,  false) to Pair(AxisId.RY, false),
            Pair(MotionEvent.AXIS_RZ,  true)  to Pair(AxisId.RY, true),
        ),
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Nintendo Switch Pro Controller (USB or Bluetooth via Android)
    // ─────────────────────────────────────────────────────────────────────────

    private val proControllerProfile = ControllerProfile(
        namePatterns = listOf("pro controller", "nintendo switch pro"),
        // Pro Controller on Android maps natively — no cross-layout swap needed
        buttons = mapOf(
            KeyEvent.KEYCODE_BUTTON_A      to ButtonId.A,
            KeyEvent.KEYCODE_BUTTON_B      to ButtonId.B,
            KeyEvent.KEYCODE_BUTTON_X      to ButtonId.X,
            KeyEvent.KEYCODE_BUTTON_Y      to ButtonId.Y,
            KeyEvent.KEYCODE_BUTTON_L1     to ButtonId.L,
            KeyEvent.KEYCODE_BUTTON_R1     to ButtonId.R,
            KeyEvent.KEYCODE_BUTTON_L2     to ButtonId.ZL,
            KeyEvent.KEYCODE_BUTTON_R2     to ButtonId.ZR,
            KeyEvent.KEYCODE_BUTTON_THUMBL to ButtonId.LeftStick,
            KeyEvent.KEYCODE_BUTTON_THUMBR to ButtonId.RightStick,
            KeyEvent.KEYCODE_BUTTON_START  to ButtonId.Plus,
            KeyEvent.KEYCODE_BUTTON_SELECT to ButtonId.Minus,
            KeyEvent.KEYCODE_BUTTON_MODE   to ButtonId.Menu,
            KeyEvent.KEYCODE_DPAD_UP       to ButtonId.DpadUp,
            KeyEvent.KEYCODE_DPAD_DOWN     to ButtonId.DpadDown,
            KeyEvent.KEYCODE_DPAD_LEFT     to ButtonId.DpadLeft,
            KeyEvent.KEYCODE_DPAD_RIGHT    to ButtonId.DpadRight,
        ),
        axes = standardAxes,
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // 8BitDo controllers
    // Covers: SN30 Pro, Pro 2, Pro+, SF30 Pro, M30, Zero 2, Lite 2, Ultimate
    // 8BitDo Android mode (X-input) uses standard Android keycodes
    // ─────────────────────────────────────────────────────────────────────────

    private val bitdoProfile = ControllerProfile(
        namePatterns = listOf("8bitdo", "8 bitdo", "sn30", "sf30", "m30 gamepad"),
        buttons = standardButtons,
        axes = standardAxes,
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Razer controllers
    // Razer Kishi, Razer Raiju, Razer Wolverine
    // ─────────────────────────────────────────────────────────────────────────

    private val razerProfile = ControllerProfile(
        namePatterns = listOf("razer", "kishi", "raiju", "wolverine"),
        buttons = standardButtons,
        axes = standardAxes,
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // GameSir controllers
    // GameSir T4 Pro, G4s, X2, X3
    // ─────────────────────────────────────────────────────────────────────────

    private val gamesirProfile = ControllerProfile(
        namePatterns = listOf("gamesir", "game sir", "t4 pro", "g4s"),
        buttons = standardButtons,
        axes = standardAxes,
        rumbleSupported = true
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Backbone One (PlayStation & Standard editions)
    // ─────────────────────────────────────────────────────────────────────────

    private val backboneProfile = ControllerProfile(
        namePatterns = listOf("backbone"),
        buttons = standardButtons,
        axes = standardAxes,
        rumbleSupported = false   // Backbone One has no rumble motors
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Generic / fallback — matches any HID gamepad that Android recognises
    // as SOURCE_GAMEPAD. Applied last if no specific profile matches.
    // ─────────────────────────────────────────────────────────────────────────

    val genericProfile = ControllerProfile(
        namePatterns = emptyList(),  // matched explicitly as fallback
        buttons = standardButtons,
        axes = standardAxes,
        rumbleSupported = false
    )

    // ─────────────────────────────────────────────────────────────────────────
    // Profile registry — order matters: first match wins
    // ─────────────────────────────────────────────────────────────────────────

    val profiles : List<ControllerProfile> = listOf(
        proControllerProfile,
        xboxProfile,
        playstationProfile,
        bitdoProfile,
        razerProfile,
        gamesirProfile,
        backboneProfile,
    )

    /**
     * Finds the best matching profile for a physical device name.
     * Returns [genericProfile] if no specific profile matches.
     */
    fun findProfile(deviceName : String) : ControllerProfile {
        val lower = deviceName.lowercase()
        return profiles.firstOrNull { profile ->
            profile.namePatterns.any { pattern -> lower.contains(pattern) }
        } ?: genericProfile
    }

    /**
     * Builds the [eventMap] entries for a given [profile] bound to a specific
     * physical [descriptor] and guest controller [controllerId].
     *
     * All existing mappings for this controller are replaced.
     */
    fun buildEventMap(
        profile : ControllerProfile,
        descriptor : String,
        controllerId : Int,
        existingMap : HashMap<HostEvent?, GuestEvent?>
    ) {
        // Remove previous mappings for this controller
        existingMap.entries
            .filter { (_, v) -> v is GuestEvent && v.id == controllerId }
            .forEach { existingMap.remove(it.key) }

        // Apply button mappings
        for ((keyCode, buttonId) in profile.buttons) {
            existingMap[KeyHostEvent(descriptor, keyCode)] =
                ButtonGuestEvent(controllerId, buttonId)
        }

        // Apply axis mappings
        for ((hostAxisPair, guestAxisPair) in profile.axes) {
            val (hostAxis, hostPolarity) = hostAxisPair
            val (guestAxis, guestPolarity) = guestAxisPair
            existingMap[MotionHostEvent(descriptor, hostAxis, hostPolarity)] =
                AxisGuestEvent(controllerId, guestAxis, guestPolarity)
        }
    }
}
