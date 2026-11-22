package dev.stockglass.auto.testmedia;

import android.media.MediaDescription;
import android.media.browse.MediaBrowser.MediaItem;
import android.net.Uri;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * A made-up music library with the shape of a streaming app's (Spotify, YouTube
 * Music), for mapping how Android Auto draws a media app's browse screens:
 *
 *   root
 *     Home       (tab) groups: "Jump back in" (grid of albums),
 *                      "Made for you" (list of playlists), "Recently played" (tracks)
 *     Playlists  (tab) 30 playlists as a list; each holds 40 tracks
 *     Albums     (tab) 40 albums as a grid; each holds 12 tracks
 *     Genres     (tab) 14 genres as a category list; each a grid of 9 albums
 *   search: tracks and albums whose names contain the query
 *
 * Ids: "tab:<name>", "pl:<n>", "al:<n>", "gn:<n>", and "t:<album>:<n>" for a
 * playable track (track n of album <album>). Everything is generated from the
 * numbers, so the tree is the same on every run. Artwork is served by
 * ArtProvider at content://dev.stockglass.auto.testmedia.art/<kind>/<seed>.
 */
final class Catalog {
    // androidx.media.utils.MediaConstants, spelled out (no androidx here).
    static final String STYLE_SUPPORTED = "android.media.browse.CONTENT_STYLE_SUPPORTED";
    static final String STYLE_BROWSABLE = "android.media.browse.CONTENT_STYLE_BROWSABLE_HINT";
    static final String STYLE_PLAYABLE = "android.media.browse.CONTENT_STYLE_PLAYABLE_HINT";
    static final String STYLE_SINGLE = "android.media.browse.CONTENT_STYLE_SINGLE_ITEM_HINT";
    static final String GROUP_TITLE = "android.media.browse.CONTENT_STYLE_GROUP_TITLE_HINT";
    static final String SEARCH_SUPPORTED = "android.media.browse.SEARCH_SUPPORTED";
    static final String EXPLICIT = "android.media.IS_EXPLICIT";
    static final String DOWNLOAD = "android.media.extra.DOWNLOAD_STATUS";
    static final String COMPLETION = "android.media.extra.PLAYBACK_STATUS";
    static final String COMPLETION_PCT = "androidx.media.MediaItem.Extras.COMPLETION_PERCENTAGE";
    static final int LIST = 1, GRID = 2, CATEGORY_LIST = 3, CATEGORY_GRID = 4;

    static final int PLAYLISTS = 30, ALBUMS = 40, GENRES = 14;
    static final int PLAYLIST_TRACKS = 40, ALBUM_TRACKS = 12, GENRE_ALBUMS = 9;

    private static final String[] ADJ = {"Liquid", "Specular", "Refracted", "Frosted", "Tinted", "Caustic",
        "Prismatic", "Polished", "Molten", "Clear", "Brushed", "Silvered", "Dispersed", "Lensing", "Glassy",
        "Opal", "Bevelled", "Mirrored", "Translucent", "Crystal"};
    private static final String[] NOUN = {"Light", "Highlights", "Normals", "Edges", "Horizon", "Weather",
        "Afternoon", "Echoes", "Signals", "Motion", "Tides", "Coastline", "Streets", "Gardens", "Static",
        "Satellites", "Mornings", "Rooms", "Rivers", "Engines"};
    private static final String[] ARTISTS = {"The Lensing Band", "Snell & the Normals", "Fresnel",
        "Kawase Quartet", "Index 1.5", "The Specular Highlights", "Dispersion", "Gaussian Blur",
        "Total Internal", "Caustics", "Prism Club", "The Rim Lights"};
    static final String[] GENRE_NAMES = {"Pop", "Rock", "Jazz", "Classical", "Electronic", "Hip-hop",
        "Ambient", "Folk", "Soul", "Metal", "Lo-fi", "Country", "Latin", "Podcasts"};
    private static final String[] PLAYLIST_NAMES = {"Daily Mix 1", "Daily Mix 2", "Daily Mix 3",
        "Discover Weekly", "Release Radar", "Liked Songs", "Drive Time", "Road Trip", "Late Night Drive",
        "Commute", "Focus", "Chill Vibes", "Morning Coffee", "Workout", "Throwbacks", "On Repeat",
        "Repeat Rewind", "Summer Hits", "Rainy Day", "Deep Focus", "Indie Mix", "Acoustic", "Party",
        "Sleep", "Top 50 Global", "Viral 50", "Mood Booster", "Golden Hour", "Night Owl", "Sunday Slow"};

    private Catalog() {}

    static String albumTitle(int a) {
        return ADJ[(a * 7 + 3) % ADJ.length] + " " + NOUN[(a * 11 + 5) % NOUN.length];
    }

    static String albumArtist(int a) {
        return ARTISTS[(a * 5 + 1) % ARTISTS.length];
    }

    static String trackTitle(int a, int n) {
        if (a == 0 && n < 3) return new String[] {"Refraction", "Specular Highlights", "Total Internal Reflection"}[n];
        return NOUN[(a * 3 + n * 7) % NOUN.length] + " " + (n % 3 == 0 ? "of " : "in ")
                + ADJ[(a + n * 13) % ADJ.length] + " " + NOUN[(a * 17 + n) % NOUN.length];
    }

    /** A playlist's track k: from a spread of albums. */
    static int[] playlistTrack(int p, int k) {
        int a = (p * 13 + k * 7) % ALBUMS;
        int n = (p + k * 5) % ALBUM_TRACKS;
        return new int[] {a, n};
    }

    static int genreAlbum(int g, int k) {
        return (g * 3 + k * 5) % ALBUMS;
    }

    static Uri art(String kind, int seed) {
        return Uri.parse("content://" + ArtProvider.AUTHORITY + "/" + kind + "/" + seed);
    }

    static List<MediaItem> children(String parent) {
        List<MediaItem> out = new ArrayList<>();
        if ("root".equals(parent)) {
            // Tabs: the last argument is the style of the tab's children.
            out.add(browsable("tab:home", "Home", null, art("cat", 100), LIST, null, -1));
            out.add(browsable("tab:playlists", "Playlists", null, art("cat", 101), LIST, null, LIST));
            out.add(browsable("tab:albums", "Albums", null, art("cat", 102), LIST, null, GRID));
            out.add(browsable("tab:genres", "Genres", null, art("cat", 103), LIST, null, CATEGORY_LIST));
        } else if ("tab:home".equals(parent)) {
            for (int a = 0; a < 6; a++) {
                out.add(withGroup(album(a, GRID), "Jump back in"));
            }
            for (int p = 0; p < 5; p++) {
                out.add(withGroup(playlist(p, LIST), "Made for you"));
            }
            for (int k = 0; k < 8; k++) {
                int a = (k * 9) % ALBUMS;
                out.add(withGroup(track(a, k % ALBUM_TRACKS, k), "Recently played"));
            }
        } else if ("tab:playlists".equals(parent)) {
            for (int p = 0; p < PLAYLISTS; p++) out.add(playlist(p, LIST));
        } else if ("tab:albums".equals(parent)) {
            for (int a = 0; a < ALBUMS; a++) out.add(album(a, GRID));
        } else if ("tab:genres".equals(parent)) {
            for (int g = 0; g < GENRES; g++) {
                out.add(browsable("gn:" + g, GENRE_NAMES[g], null, art("cat", g), CATEGORY_LIST, null, GRID));
            }
        } else if (parent.startsWith("pl:")) {
            int p = num(parent, 1);
            for (int k = 0; k < PLAYLIST_TRACKS; k++) {
                int[] t = playlistTrack(p, k);
                out.add(track(t[0], t[1], k));
            }
        } else if (parent.startsWith("al:")) {
            int a = num(parent, 1);
            for (int n = 0; n < ALBUM_TRACKS; n++) out.add(track(a, n, n));
        } else if (parent.startsWith("gn:")) {
            int g = num(parent, 1);
            for (int k = 0; k < GENRE_ALBUMS; k++) out.add(album(genreAlbum(g, k), GRID));
        }
        return out;
    }

    static List<MediaItem> search(String query) {
        String q = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
        List<MediaItem> out = new ArrayList<>();
        for (int a = 0; a < ALBUMS && out.size() < 40; a++) {
            if (q.isEmpty() || albumTitle(a).toLowerCase(Locale.ROOT).contains(q)
                    || albumArtist(a).toLowerCase(Locale.ROOT).contains(q)) {
                out.add(withGroup(album(a, LIST), "Albums"));
            }
        }
        for (int a = 0; a < ALBUMS && out.size() < 80; a++) {
            for (int n = 0; n < ALBUM_TRACKS && out.size() < 80; n++) {
                if (q.isEmpty() || trackTitle(a, n).toLowerCase(Locale.ROOT).contains(q)) {
                    out.add(withGroup(track(a, n, a * ALBUM_TRACKS + n), "Songs"));
                }
            }
        }
        return out;
    }

    static int num(String id, int part) {
        try {
            return Integer.parseInt(id.split(":")[part]);
        } catch (RuntimeException e) {
            return 0;
        }
    }

    private static MediaItem album(int a, int style) {
        return browsable("al:" + a, albumTitle(a), albumArtist(a), art("art", 1000 + a), style, null, LIST);
    }

    private static MediaItem playlist(int p, int style) {
        String sub = PLAYLIST_TRACKS + " songs · " + ARTISTS[p % ARTISTS.length] + ", "
                + ARTISTS[(p + 4) % ARTISTS.length] + " and more";
        return browsable("pl:" + p, PLAYLIST_NAMES[p % PLAYLIST_NAMES.length], sub, art("art", 2000 + p), style, null, LIST);
    }

    /** A playable track; {@code k} varies its badges (explicit, downloaded, progress). */
    static MediaItem track(int a, int n, int k) {
        Bundle e = new Bundle();
        if (k % 4 == 1) e.putLong(EXPLICIT, 1);
        if (k % 5 == 2) e.putLong(DOWNLOAD, 2);
        if (k % 7 == 3) {
            e.putInt(COMPLETION, 1);
            e.putDouble(COMPLETION_PCT, 0.4);
        }
        MediaDescription d = new MediaDescription.Builder()
                .setMediaId("t:" + a + ":" + n)
                .setTitle(trackTitle(a, n))
                .setSubtitle(albumArtist(a) + " · " + albumTitle(a))
                .setIconUri(art("art", 1000 + a))
                .setExtras(e)
                .build();
        return new MediaItem(d, MediaItem.FLAG_PLAYABLE);
    }

    private static MediaItem browsable(String id, String title, String sub, Uri icon, int single, String group,
            int childStyle) {
        Bundle e = new Bundle();
        e.putInt(STYLE_SINGLE, single);
        if (childStyle > 0) {
            e.putInt(STYLE_BROWSABLE, childStyle);
            e.putInt(STYLE_PLAYABLE, LIST);
        }
        if (group != null) e.putString(GROUP_TITLE, group);
        MediaDescription.Builder b = new MediaDescription.Builder().setMediaId(id).setTitle(title).setExtras(e);
        if (sub != null) b.setSubtitle(sub);
        if (icon != null) b.setIconUri(icon);
        return new MediaItem(b.build(), MediaItem.FLAG_BROWSABLE);
    }

    private static MediaItem withGroup(MediaItem item, String group) {
        MediaDescription d = item.getDescription();
        Bundle e = d.getExtras() == null ? new Bundle() : new Bundle(d.getExtras());
        e.putString(GROUP_TITLE, group);
        MediaDescription nd = new MediaDescription.Builder().setMediaId(d.getMediaId()).setTitle(d.getTitle())
                .setSubtitle(d.getSubtitle()).setIconUri(d.getIconUri()).setExtras(e).build();
        return new MediaItem(nd, item.getFlags());
    }
}
