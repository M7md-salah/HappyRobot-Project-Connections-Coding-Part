<style>
  .timeline-wrapper {
    width: 100%;
    overflow-x: auto; /* Enables custom scroll if it exceeds screen width */
    background-color: transparent;
    font-family: ui-monospace, 'Menlo', 'Monaco', 'Cascadia Mono', 'Consolas', monospace;
    /* Custom HUD Scrollbar */
    scrollbar-width: thin;
    scrollbar-color: rgba(245, 158, 11, 0.4) transparent;
  }

  /* Webkit Scrollbar */
  .timeline-wrapper::-webkit-scrollbar {
    height: 6px;
  }
  .timeline-wrapper::-webkit-scrollbar-track {
    background: rgba(245, 158, 11, 0.05);
    border-radius: 3px;
  }
  .timeline-wrapper::-webkit-scrollbar-thumb {
    background: rgba(245, 158, 11, 0.4);
    border-radius: 3px;
  }
  .timeline-wrapper::-webkit-scrollbar-thumb:hover {
    background: rgba(245, 158, 11, 0.7);
  }

  .timeline-track {
    display: flex;
    gap: 32px;
    position: relative;
    /* Generates the vertical space needed for the alternating squares */
    padding: 220px 0; 
    min-width: max-content;
    align-items: center;
  }

  /* The Central Axis Line (Starts center of first block, ends center of last) */
  .timeline-track::before {
    content: '';
    position: absolute;
    top: 50%;
    /* 90px is exactly half of the 180px block width */
    left: 90px; 
    right: 90px; 
    height: 1px;
    border-top: 1px dashed rgba(245, 158, 11, 0.35);
    transform: translateY(-50%);
    z-index: 0;
  }

  /* Structural Anchor for each node/block pair */
  .anchor {
    width: 180px;
    position: relative;
    display: flex;
    justify-content: center;
    z-index: 1;
  }

  /* The Glowing Node */
  .node {
    box-sizing: border-box;
    width: 12px;
    height: 12px;
    background-color: #050505;
    border: 2px solid rgba(245, 158, 11, 0.4);
    border-radius: 50%;
    position: absolute;
    top: -6px; /* perfectly centers the 12px dot on the axis line */
    left: 50%;
    transform: translateX(-50%);
    transition: all 0.3s ease;
  }

  /* Vertical Connecting Stem */
  .stem {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
    width: 1px;
    height: 24px;
    border-left: 1px dashed rgba(245, 158, 11, 0.4);
    z-index: 0;
  }

  /* The Square Block */
  .block {
    position: absolute;
    width: 180px;
    min-height: 180px; /* Forces a perfect square at rest */
    background-color: #050505;
    border: 1px solid rgba(245, 158, 11, 0.2);
    border-radius: 8px;
    padding: 20px;
    display: flex;
    flex-direction: column;
    justify-content: center;
    align-items: center;
    text-align: center;
    cursor: crosshair;
    transition: all 0.35s cubic-bezier(0.25, 1, 0.5, 1);
    box-sizing: border-box;
    z-index: 2;
  }

  /* 
    STAGGERED LOGIC: 
    Odd numbers float above the line. 
    Even numbers float below the line. 
  */
  .anchor:nth-child(odd) .block {
    bottom: 24px; /* Anchored to bottom, so hover expands it UPWARDS */
  }
  .anchor:nth-child(odd) .stem {
    bottom: 0px; 
  }

  .anchor:nth-child(even) .block {
    top: 24px; /* Anchored to top, so hover expands it DOWNWARDS */
  }
  .anchor:nth-child(even) .stem {
    top: 0px; 
  }

  /* Typography */
  .phase-id {
    color: rgba(255, 255, 255, 0.3);
    font-size: 11px;
    letter-spacing: 2px;
    margin-bottom: 8px;
    transition: all 0.3s ease;
  }
  .phase-title {
    color: rgba(245, 158, 11, 0.6);
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 1px;
    text-transform: uppercase;
    transition: all 0.3s ease;
  }
  .phase-desc {
    max-height: 0;
    opacity: 0;
    overflow: hidden;
    color: rgba(255, 255, 255, 0.7);
    font-size: 11px;
    line-height: 1.5;
    margin-top: 0;
    border-top: 1px dashed transparent;
    transition: all 0.4s cubic-bezier(0.25, 1, 0.5, 1);
  }

  /* --- INTERACTIVE HOVER STATES --- */
  
  .anchor:hover .node {
    background-color: #f59e0b;
    border-color: #f59e0b;
    box-shadow: 0 0 12px rgba(245, 158, 11, 0.8);
  }
  .anchor:hover .stem {
    border-color: #f59e0b;
  }
  
  .anchor:hover .block {
    background-color: rgba(245, 158, 11, 0.05);
    border-color: rgba(245, 158, 11, 0.6);
    box-shadow: 0 8px 25px rgba(245, 158, 11, 0.15);
  }
  
  /* Slight lift/press effect */
  .anchor:nth-child(odd):hover .block { transform: translateY(-4px); }
  .anchor:nth-child(even):hover .block { transform: translateY(4px); }

  .anchor:hover .phase-id { color: rgba(245, 158, 11, 0.8); }
  
  .anchor:hover .phase-title {
    color: #f59e0b;
    text-shadow: 0 0 8px rgba(245, 158, 11, 0.4);
  }

  .anchor:hover .phase-desc {
    max-height: 150px; /* Expands height */
    opacity: 1;
    margin-top: 12px;
    padding-top: 12px;
    border-top: 1px dashed rgba(245, 158, 11, 0.3);
  }

  /* --- PHASE 8 OVERRIDES (RED HACKATHON GLOW) --- */
  .anchor.target .node { border-color: rgba(255, 0, 60, 0.4); }
  .anchor.target .stem { border-color: rgba(255, 0, 60, 0.3); }
  .anchor.target .block { border-color: rgba(255, 0, 60, 0.2); }
  .anchor.target .phase-title { color: rgba(255, 0, 60, 0.6); }
  
  .anchor.target:hover .node {
    background-color: #ff003c;
    border-color: #ff003c;
    box-shadow: 0 0 15px rgba(255, 0, 60, 0.8);
  }
  .anchor.target:hover .stem { border-color: #ff003c; }
  
  .anchor.target:hover .block {
    background-color: rgba(255, 0, 60, 0.05);
    border-color: rgba(255, 0, 60, 0.6);
    box-shadow: 0 8px 25px rgba(255, 0, 60, 0.15);
  }
  
  .anchor.target:hover .phase-id { color: rgba(255, 0, 60, 0.8); }
  .anchor.target:hover .phase-title {
    color: #ff003c;
    text-shadow: 0 0 8px rgba(255, 0, 60, 0.4);
  }
  .anchor.target:hover .phase-desc {
    border-top: 1px dashed rgba(255, 0, 60, 0.3);
  }
</style>

<div class="timeline-wrapper">
  <div class="timeline-track">
    
    <!-- PHASE 01 (Above) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 01</div>
        <div class="phase-title">Concept &amp; CAD</div>
        <div class="phase-desc">Finalize initial sketches, component layout, and complete 3D models of the Robot Chassis.</div>
      </div>
    </div>

    <!-- PHASE 02 (Below) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 02</div>
        <div class="phase-title">Code &amp; Sims</div>
        <div class="phase-desc">Write the Code logic, test each component's logic, and simulate the project online.</div>
      </div>
    </div>

    <!-- PHASE 03 (Above) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 03</div>
        <div class="phase-title">Fabrication</div>
        <div class="phase-desc">Laser-cut the frame, 3D-print internal brackets, and assemble the main body structure.</div>
      </div>
    </div>

    <!-- PHASE 04 (Below) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 04</div>
        <div class="phase-title">Prototyping</div>
        <div class="phase-desc">Assemble components on a workbench to verify camera feeds, screen graphics, and keypad responses.</div>
      </div>
    </div>

    <!-- PHASE 05 (Above) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 05</div>
        <div class="phase-title">Final Assembly</div>
        <div class="phase-desc">Assemble the body with all the internal components, and test the features while fully integrated.</div>
      </div>
    </div>

    <!-- PHASE 06 (Below) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 06</div>
        <div class="phase-title">Integration</div>
        <div class="phase-desc">Perform field drive tests, verify remote video streaming, and refine user interface interactions.</div>
      </div>
    </div>

    <!-- PHASE 07 (Above) -->
    <div class="anchor">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 07</div>
        <div class="phase-title">Documentation</div>
        <div class="phase-desc">Finalize all technical documentation and conduct presentation rehearsals for the final pitch.</div>
      </div>
    </div>

    <!-- PHASE 08 (Below / Target Red) -->
    <div class="anchor target">
      <div class="stem"></div>
      <div class="node"></div>
      <div class="block">
        <div class="phase-id">PHASE 08</div>
        <div class="phase-title">Hackathon Prize</div>
        <div class="phase-desc">Compete in the Hackathon, secure 1st place, and take the money home.</div>
      </div>
    </div>

  </div>
</div>