// Throwaway spike for "How fast do Rough Pages arrive, and how much memory do Curls need?" (#20). Never merged.
struct MainWindow;
struct DisplayModel;

void SpikeBeforeSpreadChange(MainWindow* win, DisplayModel* dm, int newStartPage);
void SpikeAfterPaint(MainWindow* win, bool flushed);
