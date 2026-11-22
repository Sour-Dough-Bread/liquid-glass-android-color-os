package dev.stockglass.auto.testmedia;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.browse.MediaBrowser;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.SystemClock;
import android.service.media.MediaBrowserService;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/**
 * A media app for testing Android Auto's media screens: a MediaBrowserService
 * with one MediaSession that says it is playing, with a title, an artist and a
 * colourful generated artwork. It plays silence through an AudioTrack while
 * playing, with audio focus, so the system and Android Auto see a real player.
 *
 * The browse tree (Catalog) has the shape of a streaming app's: root tabs,
 * lists of playlists and albums with artwork thumbnails, a grid, a category
 * list, long lists, grouped items and search. Playing an item queues its
 * playlist or album (setQueue), and the playback state carries two custom
 * actions (shuffle, like), as Spotify's does.
 *
 *   adb shell am start -n dev.stockglass.auto.testmedia/.Start
 */
public final class Player extends MediaBrowserService {
    static final String ACTION_PLAY = "dev.stockglass.auto.testmedia.PLAY";
    private static final String TAG = "GlassTestMedia";
    private static final String CHANNEL = "player";
    private static final long DURATION_MS = 214_000;
    private static final int ART = 480;
    private static final String ACT_SHUFFLE = "shuffle", ACT_LIKE = "like";

    /** The first album's palette is the one the media card was first tested with. */
    private static final int[][] PALETTES = {
        {0xFFFF2D78, 0xFFFFB000, 0xFF00C2FF},
        {0xFF7B2DFF, 0xFF00E5A0, 0xFFFF6A00},
        {0xFF00B3FF, 0xFFFF2DD4, 0xFFFFE100},
    };

    private MediaSession session;
    private AudioManager audio;
    private AudioFocusRequest focus;
    private volatile boolean playing;
    private Thread silence;
    /** The queue: {album, track} pairs; `track` indexes it. */
    private final List<int[]> queue = new ArrayList<>();
    private int track;
    private long position;      // at `since`
    private long since;
    private boolean shuffle, liked;

    @Override
    public void onCreate() {
        super.onCreate();
        audio = getSystemService(AudioManager.class);
        session = new MediaSession(this, "GlassTestMedia");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { play(); }
            @Override public void onPause() { pause(); }
            @Override public void onSkipToNext() { skip(1); }
            @Override public void onSkipToPrevious() { skip(-1); }
            @Override public void onSeekTo(long pos) { position = pos; since = SystemClock.elapsedRealtime(); publishState(); }
            @Override public void onPlayFromMediaId(String id, Bundle extras) {
                Log.i(TAG, "play from " + id);
                playId(id);
            }
            @Override public void onPlayFromSearch(String query, Bundle extras) {
                List<MediaBrowser.MediaItem> hits = Catalog.search(query);
                for (MediaBrowser.MediaItem m : hits) {
                    if (m.isPlayable()) { playId(m.getMediaId()); return; }
                }
                playId("t:0:0");
            }
            @Override public void onSkipToQueueItem(long id) {
                if (id >= 0 && id < queue.size()) {
                    track = (int) id;
                    position = 0;
                    since = SystemClock.elapsedRealtime();
                    publishMetadata();
                    publishState();
                }
            }
            @Override public void onCustomAction(String action, Bundle extras) {
                if (ACT_SHUFFLE.equals(action)) shuffle = !shuffle;
                if (ACT_LIKE.equals(action)) liked = !liked;
                publishState();
            }
        });
        queueAlbum(0, 0);
        setSessionToken(session.getSessionToken());
        publishMetadata();
        publishState();
        session.setActive(true);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Player", NotificationManager.IMPORTANCE_LOW));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle(Catalog.trackTitle(cur()[0], cur()[1]))
                .setContentText(Catalog.albumArtist(cur()[0]))
                .setStyle(new Notification.MediaStyle().setMediaSession(session.getSessionToken()))
                .build();
        startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        if (intent != null && ACTION_PLAY.equals(intent.getAction())) play();
        return START_STICKY;
    }

    @Override
    public BrowserRoot onGetRoot(String client, int uid, Bundle hints) {
        Log.i(TAG, "onGetRoot from " + client + " hints " + hints);
        Bundle e = new Bundle();
        e.putBoolean(Catalog.STYLE_SUPPORTED, true);
        e.putInt(Catalog.STYLE_BROWSABLE, Catalog.GRID);
        e.putInt(Catalog.STYLE_PLAYABLE, Catalog.LIST);
        e.putBoolean(Catalog.SEARCH_SUPPORTED, true);
        return new BrowserRoot("root", e);
    }

    @Override
    public void onLoadChildren(String parent, Result<List<MediaBrowser.MediaItem>> result) {
        List<MediaBrowser.MediaItem> items = Catalog.children(parent);
        Log.i(TAG, "children of " + parent + ": " + items.size());
        result.sendResult(items);
    }


    /** "t:<album>:<n>" plays that track with its album queued; an album or playlist id plays its first track. */
    private void playId(String id) {
        if (id.startsWith("t:")) {
            queueAlbum(Catalog.num(id, 1), Catalog.num(id, 2));
        } else if (id.startsWith("al:")) {
            queueAlbum(Catalog.num(id, 1), 0);
        } else if (id.startsWith("pl:")) {
            int p = Catalog.num(id, 1);
            queue.clear();
            for (int k = 0; k < Catalog.PLAYLIST_TRACKS; k++) queue.add(Catalog.playlistTrack(p, k));
            track = 0;
            publishQueue();
        }
        position = 0;
        since = SystemClock.elapsedRealtime();
        publishMetadata();
        play();
        publishState();
    }

    private void queueAlbum(int a, int n) {
        queue.clear();
        for (int i = 0; i < Catalog.ALBUM_TRACKS; i++) queue.add(new int[] {a, i});
        track = Math.max(0, Math.min(n, queue.size() - 1));
        publishQueue();
    }

    private void publishQueue() {
        List<MediaSession.QueueItem> q = new ArrayList<>();
        for (int i = 0; i < queue.size(); i++) {
            int[] t = queue.get(i);
            q.add(new MediaSession.QueueItem(new MediaDescription.Builder()
                    .setMediaId("t:" + t[0] + ":" + t[1])
                    .setTitle(Catalog.trackTitle(t[0], t[1]))
                    .setSubtitle(Catalog.albumArtist(t[0]))
                    .setIconUri(Catalog.art("art", 1000 + t[0]))
                    .build(), i));
        }
        session.setQueue(q);
        session.setQueueTitle("Up next");
    }

    private int[] cur() {
        return queue.isEmpty() ? new int[] {0, 0} : queue.get(Math.max(0, Math.min(track, queue.size() - 1)));
    }

    @Override
    public void onDestroy() {
        pause();
        session.release();
        super.onDestroy();
    }

    private void play() {
        if (playing) return;
        if (focus == null) {
            focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setOnAudioFocusChangeListener(change -> {
                        if (change == AudioManager.AUDIOFOCUS_LOSS) pause();
                    })
                    .build();
        }
        int r = audio.requestAudioFocus(focus);
        Log.i(TAG, "play: audio focus " + r);
        playing = true;
        since = SystemClock.elapsedRealtime();
        silence = new Thread(this::playSilence, "silence");
        silence.start();
        session.setActive(true);
        publishState();
    }

    private void pause() {
        if (!playing) return;
        position = currentPosition();
        playing = false;
        if (silence != null) silence.interrupt();
        if (focus != null) audio.abandonAudioFocusRequest(focus);
        publishState();
    }

    private void skip(int by) {
        track = (track + by + queue.size()) % Math.max(1, queue.size());
        position = 0;
        since = SystemClock.elapsedRealtime();
        publishMetadata();
        publishState();
    }

    private long currentPosition() {
        long p = position + (playing ? SystemClock.elapsedRealtime() - since : 0);
        return Math.min(p % DURATION_MS, DURATION_MS);
    }

    private void publishState() {
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                        | PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_SKIP_TO_NEXT
                        | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SEEK_TO
                        | PlaybackState.ACTION_PLAY_FROM_MEDIA_ID | PlaybackState.ACTION_PLAY_FROM_SEARCH
                        | PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM)
                .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                        currentPosition(), playing ? 1f : 0f, SystemClock.elapsedRealtime())
                .setActiveQueueItemId(track)
                .addCustomAction(new PlaybackState.CustomAction.Builder(ACT_SHUFFLE,
                        shuffle ? "Shuffle on" : "Shuffle off", shuffle ? R.drawable.ic_shuffle_on : R.drawable.ic_shuffle).build())
                .addCustomAction(new PlaybackState.CustomAction.Builder(ACT_LIKE,
                        liked ? "Liked" : "Like", liked ? R.drawable.ic_heart_on : R.drawable.ic_heart).build())
                .build());
        Log.i(TAG, (playing ? "playing " : "paused ") + Catalog.trackTitle(cur()[0], cur()[1]));
    }

    private void publishMetadata() {
        int[] t = cur();
        int seed = 1000 + t[0];
        Bitmap art = artwork(t[0] == 0 && t[1] < 3 ? PALETTES[t[1]] : ArtProvider.palette(seed),
                t[0] == 0 && t[1] < 3 ? t[1] : seed, ART);
        session.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_MEDIA_ID, "t:" + t[0] + ":" + t[1])
                .putString(MediaMetadata.METADATA_KEY_TITLE, Catalog.trackTitle(t[0], t[1]))
                .putString(MediaMetadata.METADATA_KEY_ARTIST, Catalog.albumArtist(t[0]))
                .putString(MediaMetadata.METADATA_KEY_ALBUM, Catalog.albumTitle(t[0]))
                .putLong(MediaMetadata.METADATA_KEY_DURATION, DURATION_MS)
                .putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art)
                .putBitmap(MediaMetadata.METADATA_KEY_ART, art)
                .putBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON, art)
                .build());
    }

    /** Silence at 48 kHz, so the audio system sees a player while we say we play. */
    private void playSilence() {
        int rate = 48000;
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack t = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(Math.max(min, 8192))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        byte[] zeros = new byte[4096];
        try {
            t.play();
            while (playing && !Thread.currentThread().isInterrupted()) {
                if (t.write(zeros, 0, zeros.length) < 0) break;
            }
        } finally {
            t.stop();
            t.release();
        }
    }

    /**
     * Bold shapes and stripes, so refraction and blur over it are easy to see:
     * a three-colour gradient, two soft discs, diagonal stripes in the lower
     * half (where the card's buttons sit) and a checker band.
     */
    static Bitmap artwork(int[] c, int seed, int ART) {
        Bitmap b = Bitmap.createBitmap(ART, ART, Bitmap.Config.ARGB_8888);
        Canvas cv = new Canvas(b);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setShader(new LinearGradient(0, 0, ART, ART, c, null, Shader.TileMode.CLAMP));
        cv.drawRect(0, 0, ART, ART, p);
        p.setShader(new RadialGradient(ART * 0.3f, ART * 0.3f, ART * 0.35f,
                Color.argb(220, 255, 255, 255), Color.argb(0, 255, 255, 255), Shader.TileMode.CLAMP));
        cv.drawCircle(ART * 0.3f, ART * 0.3f, ART * 0.35f, p);
        p.setShader(null);
        p.setColor(c[2]);
        cv.drawCircle(ART * 0.75f, ART * 0.28f, ART * 0.16f, p);
        p.setColor(Color.WHITE);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(ART * 0.02f);
        cv.drawCircle(ART * 0.75f, ART * 0.28f, ART * 0.2f, p);
        p.setStyle(Paint.Style.FILL);
        // diagonal stripes over the lower half
        cv.save();
        cv.clipRect(0, ART * 0.5f, ART, ART);
        float w = ART / 16f;
        for (int i = -16; i < 32; i++) {
            p.setColor(i % 2 == 0 ? c[(i / 2 + seed + 64) % 3] : Color.argb(255, 20, 20, 30));
            cv.save();
            cv.rotate(-30, ART / 2f, ART * 0.75f);
            cv.drawRect(i * w, 0, i * w + w, ART * 2, p);
            cv.restore();
        }
        cv.restore();
        // a checker band across the middle
        float s = ART / 24f;
        for (int x = 0; x < 24; x++) {
            for (int y = 0; y < 2; y++) {
                p.setColor((x + y) % 2 == 0 ? Color.WHITE : Color.BLACK);
                cv.drawRect(x * s, ART * 0.5f - s * 2 + y * s, x * s + s, ART * 0.5f - s * 2 + y * s + s, p);
            }
        }
        return b;
    }
}
