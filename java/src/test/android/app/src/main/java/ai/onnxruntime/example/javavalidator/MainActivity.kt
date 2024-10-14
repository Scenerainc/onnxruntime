package ai.onnxruntime.example.javavalidator

import android.os.Bundle
import android.system.Os
import androidx.appcompat.app.AppCompatActivity

/*Empty activity app mainly used for testing*/
class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        Os.setenv("ADSP_LIBRARY_PATH", adspLibraryPath, true)

        super.onCreate(savedInstanceState)
    }
}
