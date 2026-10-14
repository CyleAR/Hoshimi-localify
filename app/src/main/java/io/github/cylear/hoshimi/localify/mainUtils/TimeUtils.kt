package io.github.cylear.hoshimi.localify.mainUtils

import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.time.format.DateTimeParseException
import java.time.ZoneId

object TimeUtils {
    fun convertIsoToLocalTime(isoTimeString: String?): String {
        if (isoTimeString.isNullOrBlank()) return ""
        val zonedDateTime = try {
            ZonedDateTime.parse(isoTimeString, DateTimeFormatter.ISO_DATE_TIME)
        } catch (_: DateTimeParseException) {
            return ""
        }
        val currentZoneId = ZoneId.systemDefault()
        val localZonedDateTime = zonedDateTime.withZoneSameInstant(currentZoneId)
        val outputFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
        return localZonedDateTime.format(outputFormatter)
    }
}
