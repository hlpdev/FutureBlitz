// Noticing a language server that's stopped answering: it can be running and reading, and still
// never answer, and nothing in vscode-languageclient gives up on a request by itself. Without the
// editor, so the unit tests can drive it.

export interface WatchdogOptions {
    // Asks the server something that waits for nothing. Any answer, an error too, is an answer.
    ping: () => PromiseLike<unknown>;
    // How long between pings, and how long one has to be answered.
    every: number;
    patience: number;
    // How late a timer has to go off to say this process stalled; a second, but for the tests.
    stalled?: number;
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
        this.wait();
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

    // Waits for the answer. A timer that goes off well after its time means this process was what
    // stalled, with the answer maybe waiting to be read, so the server gets its time again.
    private wait(): void {
        const due = Date.now() + this.m_options.patience;
        this.m_late = setTimeout(() => {
            if (Date.now() - due > (this.m_options.stalled ?? 1000)) {
                this.wait();
                return;
            }

            this.stop();
            this.m_options.stuck();
        }, this.m_options.patience);
    }

    private schedule(): void {
        this.m_next = setTimeout(() => this.ask(), this.m_options.every);
    }
}

// Whether a code action asked for only of this kind can be what a save waits for, as
// editor.codeActionsOnSave's are.
export function isSourceAction(kind: string | undefined): boolean {
    return kind === "source" || kind?.startsWith("source.") === true;
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
