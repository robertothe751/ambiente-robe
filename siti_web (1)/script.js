document.addEventListener('DOMContentLoaded', () => {
  const selectSSID = document.getElementById('ssid');
  const inputPass = document.getElementById('pass');
  const btnAggiornaReti = document.getElementById('btnAggiornaReti');
  const btnConnettiWifi = document.getElementById('btnConnettiWifi');
  const notifica = document.getElementById('notifica');

  function mostraNotifica(testo, tipo) {
    notifica.className = tipo;
    notifica.innerText = testo;
    notifica.removeAttribute('hidden');
  }

  let scanToken = 0;
  let scanTimer = null;

  const btnTogglePass = document.getElementById('btnTogglePass');
  if (btnTogglePass) {
    btnTogglePass.addEventListener('click', () => {
      const visibile = inputPass.type === 'text';
      inputPass.type = visibile ? 'password' : 'text';
      btnTogglePass.classList.toggle('visible', !visibile);
      btnTogglePass.setAttribute(
          'aria-label', visibile ? 'Mostra password' : 'Nascondi password');
    });
  }

  // Scansione reti Wi-Fi: polling finche' l'ESP32 segnala scanning=false
  function caricaReti(refresh = false, tentativo = 0) {
    const token = refresh || tentativo === 0 ? ++scanToken : scanToken;
    clearTimeout(scanTimer);
    if (tentativo === 0 && selectSSID.options.length <= 1) {
      selectSSID.innerHTML = '<option value="">Scansione in corso...</option>';
    }
    const riprova = (ms) => {
      scanTimer = setTimeout(() => caricaReti(false, tentativo + 1), ms);
    };
    fetch(refresh ? '/scan?refresh=1' : '/scan')
        .then(res => res.json())
        .then(data => {
          if (token !== scanToken) return;
          const networks = Array.isArray(data.networks) ? data.networks : [];
          if (data.scanning) {
            riprova(700);
            return;
          }
          if (networks.length === 0) {
            if (tentativo < 5) {
              riprova(1500);
            } else {
              selectSSID.innerHTML =
                  '<option value="">Nessuna rete trovata</option>';
            }
            return;
          }
          const currentVal = selectSSID.value;
          selectSSID.innerHTML =
              '<option value="">-- Seleziona una rete --</option>';
          const retiViste = new Set();
          networks.forEach(net => {
            const nomeRete = String(net);
            if (!nomeRete.trim() || retiViste.has(nomeRete)) return;
            retiViste.add(nomeRete);

            let opt = document.createElement('option');
            opt.value = nomeRete;
            opt.innerText = nomeRete;
            selectSSID.appendChild(opt);
          });
          if (currentVal &&
              selectSSID.querySelector(`option[value="${currentVal}"]`)) {
            selectSSID.value = currentVal;
          }
        })
        .catch(() => {
          if (token !== scanToken) return;
          if (selectSSID.options.length <= 1) {
            selectSSID.innerHTML =
                '<option value="">Errore durante la scansione</option>';
          }
          if (tentativo < 5) riprova(2000);
        });
  }

  if (btnAggiornaReti) {
    btnAggiornaReti.addEventListener('click', () => {
      selectSSID.innerHTML = '<option value="">Scansione in corso...</option>';
      caricaReti(true);
    });
  }

  // Connessione Wi-Fi
  if (btnConnettiWifi) {
    btnConnettiWifi.addEventListener('click', async () => {
      const ssid = selectSSID.value;
      const pass = inputPass.value;

      if (!ssid.trim()) {
        mostraNotifica('Seleziona prima una rete Wi-Fi.', 'error');
        return;
      }

      btnConnettiWifi.disabled = true;
      mostraNotifica('Connessione al Wi-Fi in corso...', 'info');

      try {
        const response = await fetch('/connect', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: new URLSearchParams({ssid, pass})
        });
        const data = await response.json();

        if (response.ok && data.status === 'ok') {
          mostraNotifica(
              `Wi-Fi connesso con successo! IP: ${data.ip}`, 'success');
        } else {
          throw new Error(data.message || 'Connessione Wi-Fi non riuscita.');
        }
      } catch (err) {
        mostraNotifica(`Errore Wi-Fi: ${err.message}`, 'error');
      } finally {
        btnConnettiWifi.disabled = false;
      }
    });
  }

  caricaReti();

  // Controllo periodico dello stato di connessione
  let ultimaEpoca = null;
  let ultimoCollegato = null;
  async function controllaStato() {
    try {
      const res = await fetch('/status');
      if (!res.ok) return;
      const st = await res.json();
      const cambiato = ultimaEpoca !== null &&
          (st.state_epoch !== ultimaEpoca ||
           st.network_connected !== ultimoCollegato);
      ultimaEpoca = st.state_epoch;
      ultimoCollegato = st.network_connected;
      if (!cambiato) return;

      inputPass.value = '';
      selectSSID.innerHTML = '<option value="">Scansione in corso...</option>';
      caricaReti(true);
      if (!st.network_connected) {
        mostraNotifica('Wi-Fi scollegato: seleziona una nuova rete.', 'info');
      } else {
        mostraNotifica('Wi-Fi connesso correttamente alla rete.', 'success');
      }
    } catch (e) {
    }
  }
  controllaStato();
  setInterval(controllaStato, 3000);
});