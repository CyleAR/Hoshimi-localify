package io.github.cylear.hoshimi.localify.mainUtils

import okhttp3.*
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.TimeUnit

object FileDownloader {
    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .writeTimeout(0, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.SECONDS)
        .build()

    private var call: Call? = null

    fun requestGet(request: Request, callback: Callback) {
        val client = OkHttpClient.Builder()
            .connectTimeout(30, TimeUnit.SECONDS)
            .build()
        val call = client.newCall(request)
        call.enqueue(callback)
    }

    fun downloadFileToFile(
        url: String,
        outFile: File,
        onDownload: (Float, downloaded: Long, size: Long) -> Unit,
        onSuccess: (File) -> Unit,
        onFailed: (Int, String) -> Unit,
        checkContentTypes: List<String>? = null
    ) {
        try {
            if (call != null) {
                onFailed(-1, "Another file is downloading.")
                return
            }
            outFile.parentFile?.mkdirs()
            val request = Request.Builder()
                .url(url)
                .build()

            call = client.newCall(request)
            call?.enqueue(object : Callback {
                override fun onFailure(call: Call, e: IOException) {
                    this@FileDownloader.call = null
                    outFile.delete()
                    if (call.isCanceled()) {
                        onFailed(-1, "Download canceled")
                    } else {
                        onFailed(-1, e.message ?: "Unknown error")
                    }
                }

                override fun onResponse(call: Call, response: Response) {
                    response.use {
                        if (!response.isSuccessful) {
                            this@FileDownloader.call = null
                            outFile.delete()
                            onFailed(response.code, response.message)
                            return
                        }

                        if (checkContentTypes != null) {
                            val contentType = response.header("Content-Type")
                            if (!checkContentTypes.contains(contentType)) {
                                this@FileDownloader.call = null
                                outFile.delete()
                                onFailed(-1, "Unexpected content type: $contentType")
                                return
                            }
                        }

                        val responseBody = response.body
                        if (responseBody == null) {
                            this@FileDownloader.call = null
                            outFile.delete()
                            onFailed(-1, "Response body is null")
                            return
                        }

                        val contentLength = responseBody.contentLength()
                        val buffer = ByteArray(8 * 1024)
                        var downloadedBytes = 0L
                        var read: Int

                        try {
                            responseBody.byteStream().use { input ->
                                FileOutputStream(outFile).use { output ->
                                    while (input.read(buffer).also { read = it } != -1) {
                                        output.write(buffer, 0, read)
                                        downloadedBytes += read
                                        val progress = if (contentLength < 0) {
                                            0f
                                        } else {
                                            downloadedBytes.toFloat() / contentLength
                                        }
                                        onDownload(progress, downloadedBytes, contentLength)
                                    }
                                }
                            }
                            onSuccess(outFile)
                        } catch (e: IOException) {
                            outFile.delete()
                            if (call.isCanceled()) {
                                onFailed(-1, "Download canceled")
                            } else {
                                onFailed(-1, e.message ?: "Error reading stream")
                            }
                        } finally {
                            this@FileDownloader.call = null
                        }
                    }
                }
            })
        }
        catch (e: Exception) {
            outFile.delete()
            onFailed(-1, e.toString())
            call = null
        }
    }

    fun cancel() {
        call?.cancel()
        this@FileDownloader.call = null
    }

}
