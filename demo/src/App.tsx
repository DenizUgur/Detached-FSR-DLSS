import { useEffect, useRef } from "react";
import { Player } from "./lib/playback";
import "./App.css";

function App() {
    const ref = useRef<HTMLCanvasElement>(null);
    const player = useRef<Player | null>(null);

    const handleStart = async () => {
        if (!ref.current) return;
        if (player.current) await player.current.close();

        const url = `https://${window.location.hostname}:4443`;
        const fingerprint = `${url}/fingerprint`;
        player.current = await Player.create({
            url,
            fingerprint,
            canvas: ref.current,
            namespace: "live",
        });
        player.current.play();
    };

    const handleSessionData = async () => {
        if (!player.current) return;
        const { stats, timings, skips } = await player.current.getSessionData();
        const overridenHost =
            new URL(window.location.href).searchParams.get("sinkHost") ||
            window.location.hostname;
        await fetch(`http://${overridenHost}:12000/sink`, {
            method: "POST",
            mode: "no-cors",
            headers: {
                "Content-Type": "application/json",
            },
            body: JSON.stringify({
                name: "live",
                stats,
                timings,
                skips,
            }),
        });
    };

    useEffect(() => {
        handleStart();
        return () => {
            if (player.current) player.current.close();
        };
    }, []);

    return (
        <>
            <canvas ref={ref}></canvas>
            <button onClick={handleStart} hidden>Start</button>
            <button onClick={handleSessionData} hidden>Notify Sink</button>
        </>
    );
}

export default App;
