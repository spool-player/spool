package com.sachk.spool;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.MediaMetadata;
import android.media.VolumeProvider;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

public final class RemoteMediaSessionBridge {
    private static final String CHANNEL_ID = "remote_playback";
    private static final int NOTIFICATION_ID = 71;
    private static final int NOTIFICATION_PERMISSION_REQUEST = 7171;
    private static final String ACTION_PREFIX = "com.sachk.spool.REMOTE_MEDIA_";
    private static final String EXTRA_ACTION = "action";

    private static RemoteMediaSessionBridge activeBridge;
    private static boolean localPlaybackActive;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private final Activity activity;
    private final NotificationManager notifications;
    private final MediaSession session;
    private final RemoteVolumeProvider volumeProvider;
    private boolean playing;
    private boolean notificationPermissionRequested;
    private Runnable pendingUpdate;

    public RemoteMediaSessionBridge(Activity activity)
    {
        this.activity = activity;
        this.notifications = (NotificationManager)activity.getSystemService(Context.NOTIFICATION_SERVICE);
        this.session = new MediaSession(activity, "SpoolRemoteControl");
        this.session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay()
            {
                nativeControl(0, 0);
            }
            @Override public void onPause()
            {
                nativeControl(1, 0);
            }
            @Override public void onStop()
            {
                nativeControl(3, 0);
            }
            @Override public void onSkipToNext()
            {
                nativeControl(4, 0);
            }
            @Override public void onSkipToPrevious()
            {
                nativeControl(5, 0);
            }
            @Override public void onSeekTo(long positionMs)
            {
                nativeControl(6, positionMs);
            }
            @Override public void onFastForward()
            {
                nativeControl(7, 10000);
            }
            @Override public void onRewind()
            {
                nativeControl(7, -10000);
            }
        }, new Handler(Looper.getMainLooper()));
        this.volumeProvider = new RemoteVolumeProvider();
        this.session.setPlaybackToRemote(volumeProvider);
        this.session.setSessionActivity(PendingIntent.getActivity(activity, 0,
            activity.getPackageManager().getLaunchIntentForPackage(activity.getPackageName()), pendingIntentFlags()));
        createNotificationChannel();
    }

    public void update(String title, String artist, String album, String targetName, long durationMs, long positionMs,
        boolean playing, double playbackRate, int volume)
    {
        MAIN.post(() -> {
            activeBridge = this;
            pendingUpdate = ()
                -> publish(title, artist, album, targetName, durationMs, positionMs, playing, playbackRate, volume);
            if (!localPlaybackActive)
                pendingUpdate.run();
        });
    }

    // Local playback takes the one system-control surface. Retain remote state so
    // returning to remote control does not need to wait for another server update.
    static void setLocalPlaybackActive(boolean active)
    {
        if (localPlaybackActive == active)
            return;
        localPlaybackActive = active;
        if (activeBridge == null)
            return;
        if (active)
            activeBridge.hide();
        else if (activeBridge.pendingUpdate != null)
            activeBridge.pendingUpdate.run();
    }

    private void publish(String title, String artist, String album, String targetName, long durationMs, long positionMs,
        boolean playing, double playbackRate, int volume)
    {
        activeBridge = this;
        this.playing = playing;
        volumeProvider.setCurrentVolume(Math.max(0, Math.min(100, volume)));

        MediaMetadata.Builder metadata
            = new MediaMetadata.Builder()
                  .putString(MediaMetadata.METADATA_KEY_TITLE, emptyFallback(title, "Remote playback"))
                  .putString(MediaMetadata.METADATA_KEY_ARTIST, emptyFallback(artist, targetName))
                  .putString(MediaMetadata.METADATA_KEY_ALBUM, album == null ? "" : album)
                  .putLong(MediaMetadata.METADATA_KEY_DURATION, Math.max(0, durationMs));
        session.setMetadata(metadata.build());

        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
            | PlaybackState.ACTION_STOP | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS
            | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_FAST_FORWARD | PlaybackState.ACTION_REWIND;
        PlaybackState state = new PlaybackState.Builder()
                                  .setActions(actions)
                                  .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                                      Math.max(0, positionMs), playing ? (float)playbackRate : 0.0f)
                                  .build();
        session.setPlaybackState(state);
        session.setActive(true);

        requestNotificationPermission();
        notifications.notify(NOTIFICATION_ID, buildNotification(title, targetName));
    }

    public void clear()
    {
        MAIN.post(() -> {
            pendingUpdate = null;
            hide();
            if (activeBridge == this)
                activeBridge = null;
        });
    }

    public void release()
    {
        clear();
        MAIN.post(session::release);
    }

    private void hide()
    {
        session.setPlaybackState(new PlaybackState.Builder().setState(PlaybackState.STATE_STOPPED, 0, 0.0f).build());
        session.setMetadata(null);
        session.setActive(false);
        notifications.cancel(NOTIFICATION_ID);
    }

    private Notification buildNotification(String title, String targetName)
    {
        Notification.Builder builder = new Notification.Builder(activity, CHANNEL_ID)
                                           .setSmallIcon(com.sachk.spool.R.mipmap.ic_launcher)
                                           .setContentTitle(emptyFallback(title, "Remote playback"))
                                           .setContentText("Playing on " + emptyFallback(targetName, "Jellyfin client"))
                                           .setOnlyAlertOnce(true)
                                           .setOngoing(playing)
                                           .setVisibility(Notification.VISIBILITY_PUBLIC)
                                           .setCategory(Notification.CATEGORY_TRANSPORT)
                                           .setStyle(new Notification.MediaStyle()
                                                   .setMediaSession(session.getSessionToken())
                                                   .setShowActionsInCompactView(0, 1, 2));

        builder.addAction(
            new Notification.Action.Builder(android.R.drawable.ic_media_previous, "Previous", controlIntent(5))
                .build());
        builder.addAction(new Notification.Action
                .Builder(playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                    playing ? "Pause" : "Play", controlIntent(playing ? 1 : 0))
                .build());
        builder.addAction(
            new Notification.Action.Builder(android.R.drawable.ic_media_next, "Next", controlIntent(4)).build());
        builder.addAction(
            new Notification.Action.Builder(android.R.drawable.ic_menu_close_clear_cancel, "Stop", controlIntent(3))
                .build());
        return builder.build();
    }

    private PendingIntent controlIntent(int action)
    {
        Intent intent = new Intent(activity, ControlReceiver.class)
                            .setAction(ACTION_PREFIX + action)
                            .putExtra(EXTRA_ACTION, action);
        return PendingIntent.getBroadcast(activity, action, intent, pendingIntentFlags());
    }

    private static int pendingIntentFlags()
    {
        return PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
    }

    private void createNotificationChannel()
    {
        NotificationChannel channel
            = new NotificationChannel(CHANNEL_ID, "Remote playback", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Controls the Jellyfin client selected in Spool");
        channel.setShowBadge(false);
        notifications.createNotificationChannel(channel);
    }

    private void requestNotificationPermission()
    {
        if (Build.VERSION.SDK_INT >= 33 && !notificationPermissionRequested
            && activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            notificationPermissionRequested = true;
            activity.requestPermissions(
                new String[] { Manifest.permission.POST_NOTIFICATIONS }, NOTIFICATION_PERMISSION_REQUEST);
        }
    }

    private static String emptyFallback(String value, String fallback)
    {
        return value == null || value.trim().isEmpty() ? fallback : value;
    }

    private final class RemoteVolumeProvider extends VolumeProvider {
        RemoteVolumeProvider()
        {
            super(VolumeProvider.VOLUME_CONTROL_ABSOLUTE, 100, 100);
        }

        @Override public void onSetVolumeTo(int volume)
        {
            int next = Math.max(0, Math.min(100, volume));
            setCurrentVolume(next);
            nativeControl(8, next);
        }

        @Override public void onAdjustVolume(int direction)
        {
            int next = Math.max(0, Math.min(100, getCurrentVolume() + direction * 5));
            setCurrentVolume(next);
            nativeControl(8, next);
        }
    }

    public static final class ControlReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent)
        {
            RemoteMediaSessionBridge bridge = activeBridge;
            if (bridge != null && !localPlaybackActive)
                nativeControl(intent.getIntExtra(EXTRA_ACTION, 2), 0);
        }
    }

    private static native void nativeControl(int action, long value);
}
