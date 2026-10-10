package com.sachk.spool;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.Settings;
import android.util.Log;
import androidx.core.content.FileProvider;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.Comparator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import java.io.File;

public final class AndroidUpdateBridge {
    private static final String TAG = "SpoolUpdate";
    private static final String APK_MIME_TYPE = "application/vnd.android.package-archive";

    private AndroidUpdateBridge() { }

    public static boolean canRequestPackageInstalls(Context context)
    {
        return context != null && context.getPackageManager().canRequestPackageInstalls();
    }

    public static boolean openInstallSettings(Context context)
    {
        if (context == null)
            return false;
        Intent intent
            = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + context.getPackageName()));
        return startActivity(context, intent);
    }

    public static boolean installApk(Context context, String path)
    {
        if (context == null || path == null)
            return false;
        try {
            File apk = new File(path);
            if (!apk.isFile())
                return false;
            Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + ".qtprovider", apk);
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, APK_MIME_TYPE);
            intent.setClipData(ClipData.newRawUri("Spool update", uri));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return startActivity(context, intent);
        } catch (RuntimeException exception) {
            Log.e(TAG, "Could not launch APK installer", exception);
            return false;
        }
    }

    public static boolean shareDiagnostics(Context context, String reportPath)
    {
        if (context == null || reportPath == null)
            return false;
        try {
            File report = new File(reportPath);
            File logs = report.getParentFile();
            if (!report.isFile() || logs == null || !logs.isDirectory())
                return false;

            File exportDirectory = new File(context.getCacheDir(), "diagnostics");
            if (!exportDirectory.isDirectory() && !exportDirectory.mkdirs())
                return false;
            File archive = new File(exportDirectory, "Spool-Diagnostics.zip");
            writeZip(logs, archive);

            Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + ".qtprovider", archive);
            Intent intent = new Intent(Intent.ACTION_SEND);
            intent.setType("application/zip");
            intent.putExtra(Intent.EXTRA_STREAM, uri);
            intent.putExtra(Intent.EXTRA_SUBJECT, "Spool diagnostics");
            intent.setClipData(ClipData.newRawUri("Spool diagnostics", uri));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return startActivity(context, Intent.createChooser(intent, "Share diagnostics"));
        } catch (IOException | RuntimeException exception) {
            Log.e(TAG, "Could not share diagnostics", exception);
            return false;
        }
    }

    private static void writeZip(File root, File archive) throws IOException
    {
        try (ZipOutputStream output = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(archive)))) {
            addDirectoryToZip(root, root, output);
        }
    }

    private static void addDirectoryToZip(File root, File directory, ZipOutputStream output) throws IOException
    {
        File[] files = directory.listFiles();
        if (files == null)
            throw new IOException("Could not read diagnostics directory");
        Arrays.sort(files, Comparator.comparing(File::getName));
        byte[] buffer = new byte[64 * 1024];
        for (File file : files) {
            if (file.equals(root) || file.equals(directory))
                continue;
            if (file.isDirectory()) {
                addDirectoryToZip(root, file, output);
                continue;
            }
            if (!file.isFile())
                continue;
            String entryName = root.toPath().relativize(file.toPath()).toString().replace(File.separatorChar, '/');
            output.putNextEntry(new ZipEntry(entryName));
            try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(file))) {
                int count;
                while ((count = input.read(buffer)) != -1)
                    output.write(buffer, 0, count);
            }
            output.closeEntry();
        }
    }

    private static boolean startActivity(Context context, Intent intent)
    {
        try {
            // Package visibility can hide a valid handler from resolveActivity.
            // Attempt the launch and let Android report whether it is available.
            if (!(context instanceof Activity))
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return true;
        } catch (RuntimeException exception) {
            Log.e(TAG, "Could not launch Android activity", exception);
            return false;
        }
    }
}
