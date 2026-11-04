// Noticing a language server that's stopped answering: it can be running and reading, and still
// never answer, and nothing in vscode-languageclient gives up on a request by itself. Without the
// editor, so the unit tests can drive it.

export interface WatchdogOptions {
    // Asks the server something that waits for nothing. Any answer, an error too, is an answer.
    ping: () => PromiseLike<unknown>;
    // How long between pings, and how long one has to be answered.
    every: number;
    patience: number;
    // The server didn't answer: called once, and the watchdog stops.
    stuck: () => void;
}

export class Watchdog {
    private m_next: NodeJS.Timeout | undefined;
    private m_late: NodeJS.Timeout | undefined;
    private m_asking = false;
    private m_stopped = false;

    constructor(private readonly m_options: WatchdogOptions) {
        this.schedule();
    }

    // Asks now, as what's just gone unanswered gives a reason to.
    ask(): void {
        if (this.m_stopped || this.m_asking) {
            return;
        }

        clearTimeout(this.m_next);
        this.m_asking = true;
        this.m_late = setTimeout(() => {
            this.stop();
            this.m_options.stuck();
        }, this.m_options.patience);
        const answered = (): void => {
            clearTimeout(this.m_late);
            this.m_asking = false;
            if (!this.m_stopped) {
                this.schedule();
            }
        };
        try {
            this.m_options.ping().then(answered, answered);
        } catch {
            // A client that can't send isn't the server's silence: its own close says so.
            answered();
        }
    }

    stop(): void {
        this.m_stopped = true;
        clearTimeout(this.m_next);
        clearTimeout(this.m_late);
    }

    private schedule(): void {
        this.m_next = setTimeout(() => this.ask(), this.m_options.every);
    }
}

// What `work` gives, or undefined once `patience` is up, when `late` is told: what asks, a save
// above all, doesn't wait for a server that isn't answering.
export function withinTime<T>(work: PromiseLike<T> | T, patience: number, late: () => void): Promise<T | undefined> {
    return new Promise<T | undefined>((resolve, reject) => {
        const timer = setTimeout(() => {
            late();
            resolve(undefined);
        }, patience);
        Promise.resolve(work).then(
            (value) => {
                clearTimeout(timer);
                resolve(value);
            },
            (error: unknown) => {
                clearTimeout(timer);
                reject(error instanceof Error ? error : new Error(String(error)));
            },
        );
    });
}
