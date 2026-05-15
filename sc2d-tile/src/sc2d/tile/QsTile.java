package sc2d.tile;

import android.os.AsyncTask;
import android.os.Handler;
import android.os.Looper;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public class QsTile extends TileService {

    @Override
    public void onTileAdded() {
        refreshTile();
    }

    @Override
    public void onStartListening() {
        refreshTile();
    }

    @Override
    public void onStopListening() {
    }

    @Override
    public void onClick() {
        boolean running = isDaemonRunning();
        if (running) {
            setTileState(Tile.STATE_UNAVAILABLE, "...");
            AsyncTask.execute(() -> {
                runShell("su", "-c", "killall -2 sc2d");
                sleep(800);
                new Handler(Looper.getMainLooper()).post(this::refreshTile);
            });
        } else {
            setTileState(Tile.STATE_UNAVAILABLE, "...");
            AsyncTask.execute(() -> {
                runShell("su", "-c", "nohup /data/local/tmp/sc2d > /data/local/tmp/sc2d.log 2>&1 &");
                sleep(2000);
                new Handler(Looper.getMainLooper()).post(this::refreshTile);
            });
        }
    }

    private boolean isDaemonRunning() {
        return runShell("su", "-c", "pidof sc2d > /dev/null 2>&1") == 0;
    }

    private int runShell(String... cmd) {
        try {
            Process p = Runtime.getRuntime().exec(cmd);
            return p.waitFor();
        } catch (Exception e) {
            return -1;
        }
    }

    private void sleep(long ms) {
        try { Thread.sleep(ms); } catch (Exception e) {}
    }

    private void setTileState(int state, String label) {
        Tile tile = getQsTile();
        if (tile == null) return;
        tile.setState(state);
        tile.setLabel(label);
        tile.updateTile();
    }

    private void refreshTile() {
        boolean running = isDaemonRunning();
        if (running) {
            setTileState(Tile.STATE_ACTIVE, "SC2D ON");
        } else {
            setTileState(Tile.STATE_INACTIVE, "SC2D OFF");
        }
    }
}
