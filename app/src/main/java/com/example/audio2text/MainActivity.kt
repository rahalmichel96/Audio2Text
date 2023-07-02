package com.example.audio2text

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.provider.MediaStore
import android.text.method.ScrollingMovementMethod
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.work.Data
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkInfo
import androidx.work.WorkManager
import java.io.File


class MainActivity : AppCompatActivity() {
    private lateinit var selectAudioFileLauncher: ActivityResultLauncher<Intent>
    private val bufferSize = 1024 * 4
    private lateinit var myProgressBar: ProgressBar
    private lateinit var transcriptionText: TextView
    private lateinit var header: TextView
    private lateinit var selectFileButton: Button
    val CHANNEL_ID = "com.example.audio2text.TRANSCRIPTION_CHANNEL"

    // Définir l'interface pour le callback
    interface ProgressCallback {
        fun onProgress(progress: Int)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val outputDir = applicationContext.filesDir
        myProgressBar = findViewById(R.id.loader)
        transcriptionText = findViewById(R.id.resultTextView)
        header = findViewById(R.id.header)
        // Set up the ActivityResultLauncher
        selectAudioFileLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK && result.data != null) {
                val audioUri: Uri = result.data!!.data!!

                // Handle the returned Uri
                audioUri.let {
                    // Load the audio file at audioUri
                    // Supposons que vous avez l'URI du fichier d'entrée dans une variable appelée "inputUri"
                    val inputFilePath = getPathFromUri(this,it)
                    val inputFile = File(inputFilePath!!)

                    // Extraire le nom du fichier sans l'extension
                    val fileNameWithoutExtension =
                        inputFile.name.replaceFirst("[.][^.]+$".toRegex(), "")
                    // Construct a new file in the private files directory.
                    val outputFile: File = File(outputDir, "$fileNameWithoutExtension.wav")
                    Log.d("MainActivity", "Loading audio file and convert to 16kHz wav")
                    val returnCode = (applicationContext as MyApplication).convertTo16kHz(inputFilePath, outputFile.absolutePath)
                    Log.d("MainActivity", "Conversion finished")
                    if (returnCode == 0) {
                        val data = Data.Builder()
                            .putString("audioFilePath", outputFile.absolutePath)
                            .build()

                        val workRequest = OneTimeWorkRequestBuilder<TranscriptionWorker>()
                            .setInputData(data)
                            .build()

                        WorkManager.getInstance(this)
                            .getWorkInfoByIdLiveData(workRequest.id)
                            .observe(this) { workInfo ->
                                if (workInfo != null && workInfo.state == WorkInfo.State.RUNNING) {
                                    transcriptionText.visibility = View.GONE
                                    myProgressBar.visibility = View.VISIBLE
                                    val progress = workInfo.progress.getInt("Progress", 0)
                                    Log.d("TranscriptionWorker", "Progress: $progress")
                                    header.visibility = View.GONE
                                    selectFileButton.visibility = View.GONE
                                    myProgressBar.progress = progress
                                } else if (workInfo != null && workInfo.state == WorkInfo.State.SUCCEEDED) {
                                    myProgressBar.visibility = View.GONE
                                    selectFileButton.visibility = View.VISIBLE
                                    val transcriptionResult = workInfo.outputData.getString("transcription")
                                    Log.d("Final result", transcriptionResult!!)
                                    transcriptionText.apply {
                                        text = transcriptionResult
                                        visibility = View.VISIBLE
                                        movementMethod = ScrollingMovementMethod()
                                    }
                                }
                            }

                        WorkManager.getInstance(this).enqueue(workRequest)
                    } else {
                        Log.d("Erreur de conversion", "Erreur de conversion")
                    }

                }
            }
        }

        Log.d("MainActivity", "Click on button")
        // Add a click listener to the select file button
        selectFileButton = findViewById(R.id.select_file_button)
        selectFileButton.setOnClickListener {
            // Prompt the user to select an audio file
            val intent = Intent(Intent.ACTION_PICK, MediaStore.Audio.Media.EXTERNAL_CONTENT_URI)
            selectAudioFileLauncher.launch(intent)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        (applicationContext as MyApplication).freeModelJNI()
    }

    fun getPathFromUri(context: Context, uri: Uri): String? {
        // Check if the Uri is a content Uri, which is the case if it comes from the file picker.
        if (DocumentsContract.isDocumentUri(context, uri)) {
            if (isExternalStorageDocument(uri)) {
                val docId = DocumentsContract.getDocumentId(uri)
                val split = docId.split(":")
                val type = split[0]

                if ("primary".equals(type, ignoreCase = true)) {
                    return context.getExternalFilesDir(null).toString() + "/" + split[1]
                }
            } else if (isDownloadsDocument(uri)) {
                val id = DocumentsContract.getDocumentId(uri)

                if (id.startsWith("raw:")) {
                    return id.removePrefix("raw:")
                }

                val contentUri = ContentUris.withAppendedId(
                    Uri.parse("content://downloads/public_downloads"), id.toLong()
                )

                return getDataColumn(context, contentUri, null, null)
            } else if (isMediaDocument(uri)) {
                val docId = DocumentsContract.getDocumentId(uri)
                val split = docId.split(":")
                val type = split[0]

                val contentUri = when (type) {
                    "image" -> MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                    "video" -> MediaStore.Video.Media.EXTERNAL_CONTENT_URI
                    "audio" -> MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
                    else -> null
                }

                val selection = "_id=?"
                val selectionArgs = arrayOf(split[1])

                return getDataColumn(context, contentUri, selection, selectionArgs)
            }
        } else if ("content".equals(uri.scheme, ignoreCase = true)) {
            // If it's a content Uri, we can use the ContentResolver to query it.
            return getDataColumn(context, uri, null, null)
        } else if ("file".equals(uri.scheme, ignoreCase = true)) {
            // If it's a file Uri, we can just get the path part.
            return uri.path
        }

        return null
    }

    fun getDataColumn(context: Context, uri: Uri?, selection: String?, selectionArgs: Array<String>?): String? {
        context.contentResolver.query(uri!!, null, selection, selectionArgs, null)?.use {
            if (it.moveToFirst()) {
                val index = it.getColumnIndexOrThrow(MediaStore.Images.Media.DATA)
                return it.getString(index)
            }
        }
        return null
    }

    fun isExternalStorageDocument(uri: Uri): Boolean {
        return "com.android.externalstorage.documents" == uri.authority
    }

    fun isDownloadsDocument(uri: Uri): Boolean {
        return "com.android.providers.downloads.documents" == uri.authority
    }

    fun isMediaDocument(uri: Uri): Boolean {
        return "com.android.providers.media.documents" == uri.authority
    }
}
