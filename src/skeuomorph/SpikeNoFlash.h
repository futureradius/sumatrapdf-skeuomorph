// Throwaway spike for "Can the Curl layer cover a jump with no flash?" (#19). Never merged.
struct MainWindow;
struct DisplayModel;

void SpikeBeforeSpreadChange(MainWindow* win, DisplayModel* dm, int newStartPage);
void SpikeAfterPaint(MainWindow* win, bool flushed);
