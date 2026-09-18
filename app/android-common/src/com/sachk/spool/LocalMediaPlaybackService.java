package com.sachk.spool;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;
import java.util.Objects;

// The service protects the existing native player; it never creates another player.
// Future Android picture-in-picture overlay miniplayer UI should reuse this session
// and the native queue commands, not introduce a competing playback owner.
public final class LocalMediaPlaybackService extends Service {
    private static final String CHANNEL_ID = "local_playback";
    private static final int NOTIFICATION_ID = 72;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    // All state, including remote-session arbitration, lives on Android's main thread.
    private static LocalMediaPlaybackService instance;
    private static Snapshot latest;
    private static Bitmap artwork;
    private static boolean startPending;
    private static boolean stopping;

    private MediaSession session;
    private AudioManager audio;
    private AudioFocusRequest focusRequest;
    private PowerManager.WakeLock wakeLock;
    private boolean focusHeld;
    private boolean resumeOnFocusGain;
    private boolean focusPausePending;
    private boolean released;
    private boolean foreground;
    private Snapshot published;
    private Bitmap publishedArtwork;
    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent)
        {
            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction()))
                control(1, 0);
        }
    };

    private static final class Snapshot {
        final String title, artist, album;
        final long duration, position;
        final boolean playing, buffering, canNext, canPrevious;
        final double rate;

        Snapshot(String title, String artist, String album, long duration, long position, boolean playing,
            boolean buffering, double rate, boolean canNext, boolean canPrevious)
        {
            this.title = title;
            this.artist = artist;
            this.album = album;
            this.duration = Math.max(0, duration);
            this.position = Math.max(0, position);
            this.playing = playing;
            this.buffering = buffering;
            this.rate = rate;
            this.canNext = canNext;
            this.canPrevious = canPrevious;
        }
    }

    public static void update(Context context, String title, String artist, String album, long durationMs,
        long positionMs, boolean playing, boolean buffering, double playbackRate, boolean canNext, boolean canPrevious)
    {
        Context application = context.getApplicationContext();
        Snapshot snapshot = new Snapshot(
            title, artist, album, durationMs, positionMs, playing, buffering, playbackRate, canNext, canPrevious);
        MAIN.post(() -> {
            if (stopping)
                return;
            latest = snapshot;
            RemoteMediaSessionBridge.setLocalPlaybackActive(true);
            if (instance != null) {
                instance.publish();
            } else if (!startPending) {
                startPending = true;
                try {
                    application.startForegroundService(new Intent(application, LocalMediaPlaybackService.class));
                } catch (IllegalStateException | SecurityException error) {
                    startPending = false;
                    latest = null;
                    stopping = true;
                    RemoteMediaSessionBridge.setLocalPlaybackActive(false);
                    Log.e("SpoolMedia", "Cannot protect background playback", error);
                    nativeControl(3, 0);
                }
            }
        });
    }

    public static void setArtwork(byte[] encoded)
    {
        // Called on Qt's thread once per cover, never on each position update.
        // Decode before posting so Android's UI thread only publishes the bitmap.
        Bitmap cover = encoded == null ? null : BitmapFactory.decodeByteArray(encoded, 0, encoded.length);
        MAIN.post(() -> {
            artwork = cover;
            if (instance != null)
                instance.publish();
        });
    }

    public static void clear(Context context)
    {
        MAIN.post(() -> {
            latest = null;
            artwork = null;
            stopping = false;
            if (instance != null)
                instance.finish();
            RemoteMediaSessionBridge.setLocalPlaybackActive(false);
            // A pending start must still enter onStartCommand and satisfy Android's
            // foreground deadline before stopping. Its null snapshot prevents revival.
        });
    }

    @Override public void onCreate()
    {
        super.onCreate();
        instance = this;
        NotificationChannel channel
            = new NotificationChannel(CHANNEL_ID, "Music playback", NotificationManager.IMPORTANCE_LOW);
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        audio = getSystemService(AudioManager.class);
        AudioAttributes attributes = new AudioAttributes.Builder()
                                         .setUsage(AudioAttributes.USAGE_MEDIA)
                                         .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                         .build();
        focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                           .setAudioAttributes(attributes)
                           .setWillPauseWhenDucked(true)
                           .setOnAudioFocusChangeListener(this::focusChanged, MAIN)
                           .build();
        wakeLock
            = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Spool:MusicPlayback");
        wakeLock.setReferenceCounted(false);
        session = new MediaSession(this, "SpoolLocalPlayback");
        session.setPlaybackToLocal(attributes);
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay()
            {
                control(0, 0);
            }
            @Override public void onPause()
            {
                control(1, 0);
            }
            @Override public void onStop()
            {
                control(3, 0);
            }
            @Override public void onSkipToNext()
            {
                control(4, 0);
            }
            @Override public void onSkipToPrevious()
            {
                control(5, 0);
            }
            @Override public void onSeekTo(long position)
            {
                control(6, position);
            }
        }, MAIN);
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch != null)
            session.setSessionActivity(PendingIntent.getActivity(this, 0, launch, pendingIntentFlags()));
        IntentFilter noisy = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(noisyReceiver, noisy, Context.RECEIVER_NOT_EXPORTED);
        else
            registerReceiver(noisyReceiver, noisy);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId)
    {
        startPending = false;
        // Promote before requesting audio focus: Android 15+ requires a foreground
        // app or foreground service even when resuming from lock-screen controls.
        startForeground(NOTIFICATION_ID, notification(latest));
        foreground = true;
        if (latest == null)
            finish();
        else
            publish();
        // Native mpv and its queue cannot be recovered by restarting Java alone.
        return START_NOT_STICKY;
    }

    private void publish()
    {
        Snapshot state = latest;
        if (state == null || released || !foreground)
            return;
        // Position ticks need only PlaybackState, not another bitmap transfer.
        boolean metadataChanged = published == null || publishedArtwork != artwork
            || !Objects.equals(published.title, state.title) || !Objects.equals(published.artist, state.artist)
            || !Objects.equals(published.album, state.album) || published.duration != state.duration;
        if (metadataChanged)
            session.setMetadata(new MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, state.title)
                    .putString(MediaMetadata.METADATA_KEY_ARTIST, state.artist)
                    .putString(MediaMetadata.METADATA_KEY_ALBUM, state.album)
                    .putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, artwork)
                    .putLong(MediaMetadata.METADATA_KEY_DURATION, state.duration)
                    .build());
        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
            | PlaybackState.ACTION_STOP;
        if (state.canNext)
            actions |= PlaybackState.ACTION_SKIP_TO_NEXT;
        if (state.canPrevious)
            actions |= PlaybackState.ACTION_SKIP_TO_PREVIOUS;
        if (state.duration > 0)
            actions |= PlaybackState.ACTION_SEEK_TO;
        int playbackState = !state.playing ? PlaybackState.STATE_PAUSED
            : state.buffering              ? PlaybackState.STATE_BUFFERING
                                           : PlaybackState.STATE_PLAYING;
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(actions)
                .setState(playbackState, state.position, state.playing && !state.buffering ? (float)state.rate : 0.0f)
                .build());
        session.setActive(true);
        if (metadataChanged || published.playing != state.playing || published.canNext != state.canNext
            || published.canPrevious != state.canPrevious)
            getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, notification(state));
        published = state;
        publishedArtwork = artwork;
        if (state.playing) {
            if (!wakeLock.isHeld())
                wakeLock.acquire();
            if (!focusHeld && !resumeOnFocusGain && !focusPausePending) {
                focusHeld = audio.requestAudioFocus(focusRequest) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
                if (!focusHeld) {
                    focusPausePending = true;
                    nativeControl(1, 0);
                }
            }
        } else {
            focusPausePending = false;
            if (wakeLock.isHeld())
                wakeLock.release();
            if (!resumeOnFocusGain)
                abandonFocus();
        }
    }

    private void focusChanged(int change)
    {
        if (released || latest == null)
            return;
        if (change == AudioManager.AUDIOFOCUS_GAIN) {
            focusHeld = true;
            boolean resume = resumeOnFocusGain;
            resumeOnFocusGain = false;
            if (resume)
                nativeControl(0, 0);
        } else {
            focusHeld = false;
            resumeOnFocusGain = change != AudioManager.AUDIOFOCUS_LOSS && latest.playing;
            focusPausePending = true;
            if (change == AudioManager.AUDIOFOCUS_LOSS)
                abandonFocus();
            nativeControl(1, 0);
        }
    }

    private void abandonFocus()
    {
        audio.abandonAudioFocusRequest(focusRequest);
        focusHeld = false;
    }

    private void control(int action, long value)
    {
        Snapshot state = latest;
        if (released || state == null)
            return;
        if ((action == 4 && !state.canNext) || (action == 5 && !state.canPrevious)
            || (action == 6 && state.duration <= 0))
            return;
        if (action == 0 || action == 1 || action == 2 || action == 3) {
            resumeOnFocusGain = false;
            focusPausePending = false;
        }
        nativeControl(action, value);
        if (action == 3) {
            latest = null;
            stopping = true;
            finish();
            RemoteMediaSessionBridge.setLocalPlaybackActive(false);
        }
    }

    private Notification notification(Snapshot state)
    {
        boolean playing = state != null && state.playing;
        Notification.Builder builder = new Notification.Builder(this, CHANNEL_ID)
                                           .setSmallIcon(com.sachk.spool.R.mipmap.ic_launcher)
                                           .setLargeIcon(artwork)
                                           .setContentTitle(state == null ? "Music playback" : state.title)
                                           .setContentText(state == null ? "" : state.artist)
                                           .setContentIntent(session.getController().getSessionActivity())
                                           .setOnlyAlertOnce(true)
                                           .setOngoing(playing)
                                           .setVisibility(Notification.VISIBILITY_PUBLIC)
                                           .setCategory(Notification.CATEGORY_TRANSPORT);
        int playIndex = 0;
        if (state != null && state.canPrevious) {
            builder.addAction(
                new Notification.Action.Builder(android.R.drawable.ic_media_previous, "Previous", controlIntent(5))
                    .build());
            playIndex++;
        }
        builder.addAction(new Notification.Action
                .Builder(playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                    playing ? "Pause" : "Play", controlIntent(playing ? 1 : 0))
                .build());
        if (state != null && state.canNext)
            builder.addAction(
                new Notification.Action.Builder(android.R.drawable.ic_media_next, "Next", controlIntent(4)).build());
        builder.addAction(
            new Notification.Action.Builder(android.R.drawable.ic_menu_close_clear_cancel, "Stop", controlIntent(3))
                .build());
        return builder
            .setStyle(new Notification.MediaStyle()
                    .setMediaSession(session.getSessionToken())
                    .setShowActionsInCompactView(playIndex))
            .build();
    }

    private PendingIntent controlIntent(int action)
    {
        Intent intent = new Intent(this, ControlReceiver.class)
                            .setAction("com.sachk.spool.LOCAL_MEDIA_" + action)
                            .putExtra("action", action);
        return PendingIntent.getBroadcast(this, action, intent, pendingIntentFlags());
    }

    private static int pendingIntentFlags()
    {
        return PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
    }

    private void finish()
    {
        if (released)
            return;
        released = true;
        if (instance == this)
            instance = null;
        resumeOnFocusGain = false;
        abandonFocus();
        if (wakeLock.isHeld())
            wakeLock.release();
        unregisterReceiver(noisyReceiver);
        session.setActive(false);
        session.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override public void onTaskRemoved(Intent rootIntent)
    {
        // Qt's Activity owns the native runtime. Do not leave an orphan Java
        // service if the user explicitly dismisses that runtime from recents.
        control(3, 0);
        super.onTaskRemoved(rootIntent);
    }

    @Override public void onDestroy()
    {
        if (!released) {
            latest = null;
            stopping = true;
            nativeControl(3, 0);
            finish();
            RemoteMediaSessionBridge.setLocalPlaybackActive(false);
        }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent)
    {
        return null;
    }

    public static final class ControlReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent)
        {
            if (instance != null)
                instance.control(intent.getIntExtra("action", 2), 0);
        }
    }

    private static native void nativeControl(int action, long value);
}
