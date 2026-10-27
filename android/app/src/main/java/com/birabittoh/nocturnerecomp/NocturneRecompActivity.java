package com.birabittoh.nocturnerecomp;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * Thin wrapper around SDL3's SDLActivity that tells it which native libraries
 * to load.
 */
public class NocturneRecompActivity extends SDLActivity {

    private static final String TAG = "NocturneRecomp";

    @Override
    protected String[] getLibraries() {
        return new String[] {
            runtimeLibraryName(),
            "nocturnerecomp",
        };
    }

    /**
     * The SDK installs its runtime as librexruntime.so in Release but
     * librexruntimerd.so in RelWithDebInfo, so probe for the one that was
     * actually staged. Loading it here is harmless: the caller loads the same
     * name again straight after, which the linker treats as a no-op.
     */
    private static String runtimeLibraryName() {
        try {
            System.loadLibrary("rexruntime");
            return "rexruntime";
        } catch (UnsatisfiedLinkError e) {
            return "rexruntimerd";
        }
    }

    /**
     * Called from native code (host_menu.cpp) to open the back-button host
     * menu. A real AlertDialog rather than an in-engine overlay so it draws
     * as its own window above both the game surface and the on-screen touch
     * controls, which share a render target with any in-engine overlay.
     */
    public void showHostMenu(final String[] items) {
        runOnUiThread(() -> {
            new AlertDialog.Builder(NocturneRecompActivity.this)
                .setItems(items, (DialogInterface dialog, int which) -> nativeHostMenuSelect(which))
                .setOnCancelListener((DialogInterface dialog) -> nativeHostMenuSelect(-1))
                .show();
        });
    }

    /** Called from native code to report the outcome of a menu action. */
    public void showHostToast(final String message) {
        runOnUiThread(() ->
            android.widget.Toast.makeText(this, message, android.widget.Toast.LENGTH_LONG).show());
    }

    // ---------------------------------------------------------------------
    // Storage Access Framework.
    //
    // Android 11+ hides Android/data (where user_data_root lives) from file
    // managers, so importing and exporting has to cross the sandbox. SAF hands
    // back a content:// URI, which cannot be opened with fopen; rather than
    // rerouting the native save and content code onto SAF file descriptors,
    // every transfer is staged through a plain file in the cache directory and
    // only the copy at the edge speaks content://.
    // ---------------------------------------------------------------------

    private static final int REQUEST_BASE = 0x4E4D;
    private static final int REQUEST_OPEN = REQUEST_BASE, REQUEST_CREATE = REQUEST_BASE + 1;

    /** Called from native code (document_picker.cpp); answered by nativeDocumentPicked. */
    public void requestOpenDocument() {
        runOnUiThread(() -> {
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            // STFS packages, saves and mod zips have no reliable MIME type,
            // and a picker hides whatever it cannot name.
            intent.setType("*/*");
            startActivityForResult(intent, REQUEST_OPEN);
        });
    }

    /** Called from native code; answered by nativeDocumentWrite and nativeDocumentWritten. */
    public void requestCreateDocument(final String title) {
        runOnUiThread(() -> {
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            intent.putExtra(android.content.Intent.EXTRA_TITLE, title);
            startActivityForResult(intent, REQUEST_CREATE);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        if (requestCode == REQUEST_DRIVER) {
            onDriverPicked(resultCode == RESULT_OK && data != null ? data.getData() : null);
            return;
        }
        if (requestCode != REQUEST_OPEN && requestCode != REQUEST_CREATE) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final android.net.Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        // Off the UI thread: this copies files, and native blocks on the SDL
        // thread finishing the work.
        new Thread(() -> {
            if (uri == null) {
                nativeDocumentCancelled();
            } else if (requestCode == REQUEST_CREATE) {
                exportTo(uri);
            } else {
                importFrom(uri);
            }
        }, "document-transfer").start();
    }

    /** Stage the picked document as a cache file, then let native consume it. */
    private void importFrom(android.net.Uri uri) {
        File staged = new File(getCacheDir(), "document_import");
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(staged)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) {
            Log.w(TAG, "Import staging failed: " + e.getMessage());
            staged.delete();
            nativeDocumentPicked(null);
            return;
        }
        nativeDocumentPicked(staged.getAbsolutePath());
        staged.delete();
    }

    /** Let native write the file, then copy it out to the picked document. */
    private void exportTo(android.net.Uri uri) {
        String produced = nativeDocumentWrite();
        if (produced == null) {
            // Native already reported why.
            return;
        }
        boolean ok = true;
        try (InputStream in = new java.io.FileInputStream(new File(produced));
             OutputStream out = getContentResolver().openOutputStream(uri)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) {
            Log.w(TAG, "Export copy failed: " + e.getMessage());
            ok = false;
        }
        nativeDocumentWritten(ok);
    }

    // ---------------------------------------------------------------------
    // Custom GPU drivers (libadrenotools, see android_gpu_driver.cpp).
    //
    // filesDir/gpu_drivers/<name>/ holds one extracted driver package and
    // "selected" names the active one and its library on two lines. Native
    // reads these before creating the Vulkan instance, so a change applies on
    // the next start; at startup, native waits for the dialog to close.
    // ---------------------------------------------------------------------

    private static final int REQUEST_DRIVER = REQUEST_BASE - 1;

    private boolean driverDialogAtStartup;

    /** The driver this process loaded; changing away from it needs a restart. */
    private String runningDriver;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        runningDriver = selectedDriver();
    }

    private File driversRoot() {
        return new File(getFilesDir(), "gpu_drivers");
    }

    private java.util.List<String> installedDrivers() {
        java.util.List<String> names = new java.util.ArrayList<>();
        File[] dirs = driversRoot().listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (dirs != null) {
            for (File dir : dirs) names.add(dir.getName());
        }
        java.util.Collections.sort(names, String.CASE_INSENSITIVE_ORDER);
        return names;
    }

    private String selectedDriver() {
        try (java.io.BufferedReader reader = new java.io.BufferedReader(
                new java.io.FileReader(new File(driversRoot(), "selected")))) {
            return reader.readLine();
        } catch (Exception e) {
            return null;
        }
    }

    /** Null selects the system driver. */
    private void selectDriver(String name) {
        File selected = new File(driversRoot(), "selected");
        String library = name == null ? null : driverLibrary(new File(driversRoot(), name));
        if (library == null) {
            selected.delete();
            return;
        }
        try (java.io.FileWriter writer = new java.io.FileWriter(selected)) {
            writer.write(name + "\n" + library + "\n");
        } catch (Exception e) {
            Log.w(TAG, "Could not save the driver selection: " + e.getMessage());
        }
    }

    /** The driver's libraryName from meta.json, else its only .so. */
    private static String driverLibrary(File dir) {
        org.json.JSONObject meta = readDriverMeta(dir);
        if (meta != null) {
            String library = meta.optString("libraryName", "");
            if (!library.isEmpty() && new File(dir, library).isFile()) return library;
        }
        File[] libs = dir.listFiles((d, n) -> n.endsWith(".so"));
        return libs != null && libs.length == 1 ? libs[0].getName() : null;
    }

    private static org.json.JSONObject readDriverMeta(File dir) {
        try {
            byte[] bytes = java.nio.file.Files.readAllBytes(new File(dir, "meta.json").toPath());
            return new org.json.JSONObject(new String(bytes, "UTF-8"));
        } catch (Exception e) {
            return null;
        }
    }

    public void showGpuDriverDialog(final boolean atStartup) {
        runOnUiThread(() -> buildDriverDialog(atStartup));
    }

    private void buildDriverDialog(final boolean atStartup) {
        final java.util.List<String> names = installedDrivers();
        final String current = selectedDriver();
        String[] items = new String[names.size() + 1];
        items[0] = text(T_DRIVER_SYSTEM);
        for (int i = 0; i < names.size(); i++) items[i + 1] = names.get(i);
        final int[] choice = { Math.max(0, names.indexOf(current) + 1) };

        AlertDialog.Builder builder = new AlertDialog.Builder(this)
            .setTitle(text(atStartup ? T_DRIVER_TITLE_STARTUP : T_DRIVER_TITLE))
            .setSingleChoiceItems(items, choice[0], (d, which) -> choice[0] = which)
            .setPositiveButton(text(atStartup ? T_START : T_OK), (d, w) -> {
                String picked = choice[0] == 0 ? null : names.get(choice[0] - 1);
                selectDriver(picked);
                if (atStartup) {
                    runningDriver = selectedDriver();
                    nativeGpuDriverDialogClosed();
                } else {
                    offerCloseForDriver();
                }
            })
            .setNeutralButton(text(T_INSTALL), (d, w) -> {
                driverDialogAtStartup = atStartup;
                android.content.Intent intent =
                    new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, REQUEST_DRIVER);
            });
        if (!names.isEmpty()) {
            builder.setNegativeButton(text(T_DELETE), (d, w) -> {
                if (choice[0] > 0) {
                    String name = names.get(choice[0] - 1);
                    deleteTree(new File(driversRoot(), name));
                    if (name.equals(current)) selectDriver(null);
                }
                buildDriverDialog(atStartup);
            });
        }
        // At startup the game waits on this dialog, so it has to end in a button.
        builder.setCancelable(!atStartup);
        if (!atStartup) builder.setOnCancelListener(d -> offerCloseForDriver());
        builder.show();
    }

    /**
     * The driver is only loaded at startup. Android gives the game no way to
     * relaunch itself, so the most it can do is close.
     */
    private void offerCloseForDriver() {
        if (java.util.Objects.equals(selectedDriver(), runningDriver)) return;
        new AlertDialog.Builder(this)
            .setTitle(text(T_DRIVER_CHANGED_TITLE))
            .setMessage(text(T_DRIVER_CHANGED_MESSAGE))
            .setPositiveButton(text(T_CLOSE_GAME), (d, w) -> nativeCloseGame())
            .setNegativeButton(text(T_LATER), null)
            .show();
    }

    private void onDriverPicked(final android.net.Uri uri) {
        final boolean atStartup = driverDialogAtStartup;
        if (uri == null) {
            buildDriverDialog(atStartup);
            return;
        }
        new Thread(() -> {
            String installed = installDriver(uri);
            runOnUiThread(() -> {
                if (installed == null) {
                    showHostToast(text(T_DRIVER_INVALID));
                } else {
                    selectDriver(installed);
                    showHostToast(text(T_DRIVER_INSTALLED).replace("{}", installed));
                }
                buildDriverDialog(atStartup);
            });
        }, "gpu-driver-install").start();
    }

    /** Extracts a driver zip (or takes a bare .so); returns its folder name or null. */
    private String installDriver(android.net.Uri uri) {
        File staging = new File(driversRoot(), ".staging");
        deleteTree(staging);
        staging.mkdirs();
        String displayName = queryDisplayName(uri);
        try (InputStream raw = getContentResolver().openInputStream(uri);
             java.io.BufferedInputStream in = new java.io.BufferedInputStream(raw)) {
            in.mark(4);
            byte[] magic = new byte[4];
            int got = in.read(magic);
            in.reset();
            if (got == 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
                String name = displayName != null && displayName.endsWith(".so")
                    ? displayName : "vulkan.custom.so";
                copyStream(in, new File(staging, name));
            } else {
                extractZip(in, staging);
            }
        } catch (Exception e) {
            Log.w(TAG, "Driver install failed: " + e.getMessage());
            deleteTree(staging);
            return null;
        }

        // Some packages wrap everything in one folder.
        File root = staging;
        File[] top = staging.listFiles();
        if (top != null && top.length == 1 && top[0].isDirectory()) root = top[0];
        if (driverLibrary(root) == null) {
            deleteTree(staging);
            return null;
        }

        String name = "";
        org.json.JSONObject meta = readDriverMeta(root);
        if (meta != null) {
            name = (meta.optString("name", "") + " " + meta.optString("driverVersion", "")).trim();
        }
        if (name.isEmpty()) {
            name = displayName != null ? displayName.replaceFirst("\\.[^.]*$", "") : "Custom driver";
        }
        name = name.replaceAll("[^A-Za-z0-9 ._+-]", "_").replaceFirst("^\\.+", "");
        if (name.isEmpty()) name = "Custom driver";

        File dest = new File(driversRoot(), name);
        deleteTree(dest);
        boolean moved = root.renameTo(dest);
        deleteTree(staging);
        return moved ? name : null;
    }

    private String queryDisplayName(android.net.Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(
                uri, new String[] { android.provider.OpenableColumns.DISPLAY_NAME },
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (Exception ignored) {
        }
        return null;
    }

    private static void extractZip(InputStream in, File dest) throws java.io.IOException {
        String destPath = dest.getCanonicalPath() + File.separator;
        try (java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(in)) {
            java.util.zip.ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                File out = new File(dest, entry.getName());
                if (!out.getCanonicalPath().startsWith(destPath)) continue;
                if (entry.isDirectory()) {
                    out.mkdirs();
                } else {
                    out.getParentFile().mkdirs();
                    copyStream(zip, out);
                }
            }
        }
    }

    private static void copyStream(InputStream in, File out) throws java.io.IOException {
        try (OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        file.delete();
    }

    // Indices into nativeHostTexts(), in the order of JavaTexts() in host_menu.cpp.
    private static final int T_DRIVER_INVALID = 0, T_DRIVER_INSTALLED = 1, T_DRIVER_SYSTEM = 2,
        T_DRIVER_TITLE_STARTUP = 3, T_DRIVER_TITLE = 4, T_START = 5, T_OK = 6, T_INSTALL = 7,
        T_DELETE = 8, T_DRIVER_CHANGED_TITLE = 9, T_DRIVER_CHANGED_MESSAGE = 10,
        T_CLOSE_GAME = 11, T_LATER = 12;

    private String text(int index) {
        try {
            return nativeHostTexts()[index];
        } catch (Throwable e) {
            return "";
        }
    }

    private native String[] nativeHostTexts();
    private native void nativeHostMenuSelect(int index);
    private native void nativeDocumentPicked(String path);
    private native String nativeDocumentWrite();
    private native void nativeDocumentWritten(boolean ok);
    private native void nativeDocumentCancelled();
    private native void nativeGpuDriverDialogClosed();
    private native void nativeCloseGame();
}
