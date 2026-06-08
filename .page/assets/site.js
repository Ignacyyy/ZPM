(function () {
  function ready(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn);
    } else {
      fn();
    }
  }

  function initMobileMenu() {
    const toggle = document.querySelector(".menu-toggle");
    const panel = document.querySelector(".mobile-menu");
    if (!toggle || !panel) return;

    function setOpen(open) {
      toggle.classList.toggle("open", open);
      panel.classList.toggle("open", open);
      document.body.classList.toggle("menu-open", open);
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
    }

    toggle.addEventListener("click", () => {
      setOpen(!panel.classList.contains("open"));
    });

    panel.addEventListener("click", (event) => {
      if (event.target.closest("a")) setOpen(false);
    });

    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") setOpen(false);
    });

    document.addEventListener("click", (event) => {
      if (!panel.classList.contains("open")) return;
      if (event.target.closest(".mobile-menu") || event.target.closest(".menu-toggle")) return;
      setOpen(false);
    });
  }

  function initPageTransitions() {
    document.addEventListener("click", (event) => {
      const link = event.target.closest("a[href]");
      if (!link) return;
      if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      if (link.target && link.target !== "_self") return;
      if (link.hasAttribute("download")) return;

      const href = link.getAttribute("href");
      if (!href || href.startsWith("#") || href.startsWith("mailto:") || href.startsWith("tel:")) return;

      const next = new URL(href, window.location.href);
      if ((next.protocol === "http:" || next.protocol === "https:") && next.host !== window.location.host) return;
      if (next.pathname === window.location.pathname && next.hash) return;
      if (next.protocol === "file:" && next.pathname.endsWith("/")) {
        next.pathname = `${next.pathname}index.html`;
      }

      event.preventDefault();
      document.body.classList.add("is-leaving");
      window.setTimeout(() => {
        window.location.href = next.href;
      }, 180);
    });
  }

  ready(() => {
    initMobileMenu();
    initPageTransitions();
  });
})();
