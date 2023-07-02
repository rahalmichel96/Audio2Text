package com.example.audio2text

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.res.AssetManager
import android.os.Build

class MyApplication : Application() {

    companion object {
        const val CHANNEL_ID = "transcription_channel"
        init {
            System.loadLibrary("native-lib");
        }
    }

    override fun onCreate() {
        super.onCreate()

        val name = getString(R.string.channel_name)
        val descriptionText = getString(R.string.channel_description)
        val importance = NotificationManager.IMPORTANCE_DEFAULT
        val channel = NotificationChannel(CHANNEL_ID, name, importance).apply {
            description = descriptionText
        }
        val notificationManager: NotificationManager =
            getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.createNotificationChannel(channel)
    }

    external fun convertTo16kHz(inputFilePath: String?, outputFilePath: String?): Int

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    // Load model by TF Lite C++ API
    external fun loadModelJNI(
        assetManager: AssetManager,
        fileName: String,
        callback: JNIProgressCallback
    ): String?

    external fun freeModelJNI(): Int
}