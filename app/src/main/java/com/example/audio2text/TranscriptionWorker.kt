package com.example.audio2text

import android.app.NotificationManager
import android.content.Context
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.work.CoroutineWorker
import androidx.work.Data
import androidx.work.ForegroundInfo
import androidx.work.WorkerParameters
import androidx.work.workDataOf

class TranscriptionWorker(context: Context, workerParams: WorkerParameters) : CoroutineWorker(context, workerParams) {
    private val notificationManager =
        context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    override suspend fun doWork(): Result {
        // Create a ForegroundInfo for the notification
        val notification = NotificationCompat.Builder(applicationContext, CHANNEL_ID)
            .setContentTitle("Transcription en cours")
            .setTicker("Transcription en cours")
            .setSmallIcon(R.drawable.notification_icon)
            .setOngoing(true)
            .build()

        val foregroundInfo = ForegroundInfo(NOTIFICATION_ID, notification)
        setForeground(foregroundInfo)

        val audioFilePath = inputData.getString("audioFilePath")

        // Start transcription
        val transcription = startTranscription(audioFilePath)

        val outputData = Data.Builder()
            .putString("transcription", transcription)
            .build()

        return Result.success(outputData)
    }

    private suspend fun startTranscription(filePath : String?) : String? {
        // Call your JNI function here and update the notification with the progress

        val totalProgress = 100
        var currentProgress = 0

        // Define the progress callback
        val progressCallback = object: JNIProgressCallback {
            override fun onProgressUpdate(progress: Int) {
                Log.d("TranscriptionWorker", "onProgressUpdate called with progress: $progress")
                currentProgress = progress
                val progressData = workDataOf("Progress" to currentProgress)
                setProgressAsync(progressData)
                updateNotificationProgress(currentProgress)
            }
        }

        // Call the JNI function
        val transcription = filePath?.let {
            (applicationContext as MyApplication).loadModelJNI(applicationContext.assets,
                it, progressCallback)
        }

        return transcription?.replace(Regex("\\[.*?\\]"), "")
    }

    private fun updateNotificationProgress(progress: Int) {

        val notification = NotificationCompat.Builder(applicationContext, CHANNEL_ID)
            .setContentTitle("Transcription en cours")
            .setContentText("$progress% complétés")
            .setSmallIcon(R.drawable.notification_icon)
            .setProgress(100, progress, false)
            .build()

        notificationManager.notify(NOTIFICATION_ID, notification)
    }

    companion object {
        const val NOTIFICATION_ID = 1
        const val CHANNEL_ID = "transcription_channel"
    }
}
