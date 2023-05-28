package com.example.audio2text

import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.res.AssetManager
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.MediaStore
import android.util.Log
import android.widget.Button
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

import java.io.File


class MainActivity : AppCompatActivity() {
    private lateinit var selectAudioFileLauncher: ActivityResultLauncher<Intent>
    private val bufferSize = 1024 * 4

    companion object {
        init {
            System.loadLibrary("native-lib");
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val outputDir = applicationContext.filesDir


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
                    val returnCode = convertTo16kHz(inputFilePath, outputFile.absolutePath)
                    Log.d("MainActivity", "Conversion finished")
                    if (returnCode == 0) {
                        Log.d("File path", outputFile.absolutePath)
                        // Conversion réussie
                        val transcript: String? = loadModelJNI(assets,outputFile.absolutePath)
                        Log.d("Transcript", transcript.toString())
                    } else {
                        Log.d("Erreur de conversion", "Erreur de conversion")
                    }

                }
            }
        }

        Log.d("MainActivity", "Click on button")
        // Add a click listener to the select file button
        val selectFileButton: Button = findViewById(R.id.select_file_button)
        selectFileButton.setOnClickListener {
            // Prompt the user to select an audio file
            val intent = Intent(Intent.ACTION_PICK, MediaStore.Audio.Media.EXTERNAL_CONTENT_URI)
            selectAudioFileLauncher.launch(intent)
        }
    }

    private external fun convertTo16kHz(inputFilePath: String?, outputFilePath: String?): Int

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    // Load model by TF Lite C++ API
    private external fun loadModelJNI(
        assetManager: AssetManager,
        fileName: String
    ): String?

    private external fun freeModelJNI(): Int
    override fun onDestroy() {
        super.onDestroy()
        freeModelJNI()
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
