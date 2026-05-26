/**
 * Тонкий слой: рисует SVG-кривые между «выходом» датчика и «входом» ECU.
 *
 * API:
 *   const wiring = new Wiring({ svg, container });
 *   wiring.setConnections([{ from: outletEl, to: inletEl, color }]);
 *   wiring.refresh();      // вызвать при изменении размера/скролла
 */

const SVG_NS = "http://www.w3.org/2000/svg";

export class Wiring {
  constructor({ svg, container }) {
    this.svg = svg;
    this.container = container;
    this.connections = [];

    const refresh = () => this.refresh();
    window.addEventListener("resize", refresh);
    this.container.addEventListener("scroll", refresh, { passive: true });
    this._ro = new ResizeObserver(refresh);
    this._ro.observe(this.container);
  }

  setConnections(items) {
    this.connections = items.filter((c) => c.from && c.to);
    this.refresh();
  }

  refresh() {
    while (this.svg.firstChild) this.svg.removeChild(this.svg.firstChild);
    const box = this.container.getBoundingClientRect();
    this.svg.setAttribute("viewBox", `0 0 ${box.width} ${box.height}`);
    this.svg.setAttribute("width", box.width);
    this.svg.setAttribute("height", box.height);

    for (const c of this.connections) {
      const from = c.from.getBoundingClientRect();
      const to = c.to.getBoundingClientRect();
      const x1 = from.left + from.width / 2 - box.left;
      const y1 = from.top + from.height / 2 - box.top;
      const x2 = to.left + to.width / 2 - box.left;
      const y2 = to.top + to.height / 2 - box.top;
      const mid = Math.max(28, Math.abs(x2 - x1) * 0.45);
      const d = `M ${x1},${y1} C ${x1 + mid},${y1} ${x2 - mid},${y2} ${x2},${y2}`;

      const path = document.createElementNS(SVG_NS, "path");
      path.setAttribute("d", d);
      path.setAttribute("fill", "none");
      path.setAttribute("stroke", c.color || "#2b6cb0");
      path.setAttribute("stroke-width", "1.5");
      path.setAttribute("stroke-linecap", "round");
      this.svg.appendChild(path);
    }
  }
}
