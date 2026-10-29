/* SPDX-License-Identifier: MPL-2.0 */

package org.stratoemu.strato.diagnostics

import java.time.Instant
import java.time.ZoneId

/** Local calendar days including today, not fixed 24-hour intervals (DST safe). */
internal fun diagnosticExportWindow(days : Int, now : Instant = Instant.now(),
                                    zone : ZoneId = ZoneId.systemDefault()) : LongRange {
    require(days > 0)
    val today = now.atZone(zone).toLocalDate()
    val start = today.minusDays(days.toLong() - 1).atStartOfDay(zone).toInstant().toEpochMilli()
    val end = today.plusDays(1).atStartOfDay(zone).toInstant().toEpochMilli()
    return start until end
}
