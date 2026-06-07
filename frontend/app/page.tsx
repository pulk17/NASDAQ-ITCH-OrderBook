"use client";

import { useEffect, useState, useRef } from "react";

type PriceLevel = { price: number; shares: number };
type BookSnapshot = {
  timestamp: string;
  ofi: number;
  symbol: string;
  asks: PriceLevel[];
  bids: PriceLevel[];
};

export default function PitchForkDashboard() {
  const [snapshot, setSnapshot] = useState<BookSnapshot | null>(null);
  const [isConnected, setIsConnected] = useState(false);
  
  const wsRef = useRef<WebSocket | null>(null);
  
  // CRITICAL: We use a ref to store the latest network data instantly without triggering a React render
  const latestSnapshotRef = useRef<BookSnapshot | null>(null);

  useEffect(() => {
    wsRef.current = new WebSocket("ws://localhost:8080");
    wsRef.current.binaryType = "arraybuffer";

    wsRef.current.onopen = () => setIsConnected(true);
    wsRef.current.onclose = () => setIsConnected(false);

    wsRef.current.onmessage = (event) => {
      const buffer = event.data as ArrayBuffer;
      if (buffer.byteLength !== 102) return; 

      const view = new DataView(buffer);
      const tsBigInt = view.getBigUint64(0, true);
      const ofi = view.getInt32(8, true);

      const symbolBytes = new Uint8Array(buffer, 12, 8);
      // Safely decode and strip out any C++ null bytes (\0)
      const symbol = new TextDecoder().decode(symbolBytes).replace(/\0/g, '').trim();

      const numAsks = view.getUint8(20);
      const numBids = view.getUint8(21);

      const asks: PriceLevel[] = [];
      let offset = 22;
      for (let i = 0; i < Math.min(numAsks, 5); i++) {
        asks.push({
          price: view.getUint32(offset, true) / 10000,
          shares: view.getUint32(offset + 4, true),
        });
        offset += 8;
      }

      const bids: PriceLevel[] = [];
      offset = 62;
      for (let i = 0; i < Math.min(numBids, 5); i++) {
        bids.push({
          price: view.getUint32(offset, true) / 10000,
          shares: view.getUint32(offset + 4, true),
        });
        offset += 8;
      }

      // Store the payload in the mutable ref instead of React state
      latestSnapshotRef.current = {
        timestamp: tsBigInt.toString(),
        ofi,
        symbol,
        asks: asks.reverse(), 
        bids,
      };
    };

    // UI Render Loop: Pulls from the ref at a safe 60 FPS
    let animationFrameId: number;
    const renderLoop = () => {
      if (latestSnapshotRef.current) {
        setSnapshot(latestSnapshotRef.current);
      }
      animationFrameId = requestAnimationFrame(renderLoop);
    };
    renderLoop();

    return () => {
      wsRef.current?.close();
      cancelAnimationFrame(animationFrameId);
    };
  }, []);

  return (
    <div className="min-h-screen bg-gray-950 text-gray-100 p-8 font-mono">
      <div className="max-w-4xl mx-auto">
        
        <div className="flex justify-between items-center mb-8 border-b border-gray-800 pb-4">
          <div>
            <h1 className="text-3xl font-bold tracking-tight text-white">PitchFork Engine</h1>
            <p className="text-sm text-gray-400">Zero-Copy LOB & Quant Signal</p>
          </div>
          <div className="flex items-center space-x-2">
            <span className="text-sm text-gray-400">Status:</span>
            <div className={`px-3 py-1 rounded text-sm font-bold ${isConnected ? 'bg-green-500/20 text-green-400' : 'bg-red-500/20 text-red-400'}`}>
              {isConnected ? "LIVE" : "DISCONNECTED"}
            </div>
          </div>
        </div>

        {!snapshot ? (
          <div className="text-center text-gray-500 mt-20">Waiting for C++ UDP Firehose...</div>
        ) : (
          <div className="grid grid-cols-1 md:grid-cols-2 gap-8">
            
            <div className="bg-gray-900 border border-gray-800 rounded-lg p-6 shadow-xl">
              <h2 className="text-xl font-bold mb-4 border-b border-gray-800 pb-2 flex justify-between">
                <span>{snapshot.symbol || "AAPL"}</span>
                <span className="text-sm text-gray-500 font-normal">TS: {snapshot.timestamp}</span>
              </h2>
              
              <div className="grid grid-cols-2 text-sm text-gray-500 mb-2 font-bold px-2">
                <div>Price (USD)</div>
                <div className="text-right">Shares</div>
              </div>

              <div className="space-y-1 mb-4">
                {snapshot.asks.map((ask, i) => (
                  <div key={`ask-${i}`} className="grid grid-cols-2 text-red-400 bg-red-950/30 px-2 py-1 rounded">
                    <div>{ask.price.toFixed(2)}</div>
                    <div className="text-right">{ask.shares.toLocaleString()}</div>
                  </div>
                ))}
              </div>

              <div className="text-center text-gray-600 text-xs py-1 border-y border-gray-800 mb-4 bg-gray-950">
                --- SPREAD ---
              </div>

              <div className="space-y-1">
                {snapshot.bids.map((bid, i) => (
                  <div key={`bid-${i}`} className="grid grid-cols-2 text-green-400 bg-green-950/30 px-2 py-1 rounded">
                    <div>{bid.price.toFixed(2)}</div>
                    <div className="text-right">{bid.shares.toLocaleString()}</div>
                  </div>
                ))}
              </div>
            </div>

            <div className="space-y-6">
              <div className="bg-gray-900 border border-gray-800 rounded-lg p-6 shadow-xl">
                <h3 className="text-gray-400 text-sm font-bold uppercase tracking-wider mb-2">
                  Order Flow Imbalance (OFI)
                </h3>
                <div className="flex items-baseline space-x-2">
                  <span className={`text-5xl font-bold ${
                      snapshot.ofi > 0 ? "text-green-500" : 
                      snapshot.ofi < 0 ? "text-red-500" : "text-gray-300"
                    }`}>
                    {snapshot.ofi > 0 ? "+" : ""}{snapshot.ofi.toLocaleString()}
                  </span>
                </div>
                <p className="mt-4 text-sm text-gray-500">
                  Real-time net liquidity flux at the Best Bid / Best Ask. Positive OFI indicates aggressive buying pressure.
                </p>
              </div>
            </div>

          </div>
        )}
      </div>
    </div>
  );
}