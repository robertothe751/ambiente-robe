document.addEventListener('DOMContentLoaded', () => {
  const lblSsid = document.getElementById('diag-ssid');
  const lblNetworkStatus = document.getElementById('diag-network-status');
  const lblLastSync = document.getElementById('diag-last-sync');
  const lblFirmware = document.getElementById('diag-firmware');

  const FIRMWARE_VERSION = '1.0.0';

  const btnResetWifi = document.getElementById('btnResetWifi');
  const btnRebootEsp = document.getElementById('btnRebootEsp');
  const btnResetEsp = document.getElementById('btnResetEsp');
  const notifica = document.getElementById('notifica');

  let statoPrecedenteConnessione = null;
  let epocaPrecedente = null;

  function aggiornaEtichette(data) {
    const connected = data.network_connected;
    const ssidText =
        data.ssid ? data.ssid : (connected ? 'Connesso' : 'Disconnesso');
    lblSsid.textContent = `Rete: ${ssidText}`;
    lblNetworkStatus.textContent =
        `Stato Rete: ${connected ? 'Connessa' : 'Non connessa'}`;
    lblLastSync.textContent = connected ?
        'Ultima Sincronizzazione: ora' :
        `Ultima Sincronizzazione: ${
            formattaTempoTrascorso(data.elapsed_seconds || 0)}`;

    // Legge la versione in modo dinamico dal backend dell'ESP32
    lblFirmware.textContent = `Firmware: ${data.firmware || '1.0.0'}`;

    statoPrecedenteConnessione = connected;
    epocaPrecedente = data.state_epoch;
  }

  // Reset Wi-Fi
  btnResetWifi.addEventListener('click', async () => {
    btnResetWifi.disabled = true;
    notifica.className = 'info';
    notifica.innerText = 'Reset Wi-Fi in corso...';
    notifica.removeAttribute('hidden');
    try {
      const res = await fetch('/reset_wifi', {method: 'POST'});
      const data = await res.json();
      if (!res.ok || data.status !== 'ok') throw new Error('Reset fallito');
      aggiornaEtichette(data);
      notifica.className = 'success';
      notifica.innerText = 'Wi-Fi scollegato.';
    } catch (e) {
      notifica.className = 'error';
      notifica.innerText = 'Errore durante il reset Wi-Fi.';
    } finally {
      btnResetWifi.disabled = false;
    }
  });

  // Riavvio ESP32
  if (btnRebootEsp) {
    btnRebootEsp.addEventListener('click', async () => {
      btnRebootEsp.disabled = true;
      notifica.className = 'info';
      notifica.innerText = 'Riavvio dell\'ESP32 in corso...';
      notifica.removeAttribute('hidden');
      try {
        const res = await fetch('/reboot', {method: 'POST'});
        const data = await res.json();
        if (!res.ok || data.status !== 'ok') throw new Error('Riavvio fallito');
        notifica.className = 'success';
        notifica.innerText = 'Dispositivo in riavvio...';
      } catch (e) {
        notifica.className = 'error';
        notifica.innerText = 'Errore durante il riavvio del dispositivo.';
        btnRebootEsp.disabled = false;
      }
    });
  }

  // Reset ESP32 (Factory Reset)
  if (btnResetEsp) {
    btnResetEsp.addEventListener('click', async () => {
      if (!confirm(
              'Sei sicuro di voler ripristinare l\'ESP32 alle impostazioni di fabbrica? (Verranno cancellate tutte le impostazioni e le credenziali Wi-Fi)')) {
        return;
      }
      btnResetEsp.disabled = true;
      notifica.className = 'info';
      notifica.innerText = 'Reset di fabbrica in corso...';
      notifica.removeAttribute('hidden');
      try {
        const res = await fetch('/reset_factory', {method: 'POST'});
        const data = await res.json();
        if (!res.ok || data.status !== 'ok') throw new Error('Reset fallito');
        notifica.className = 'success';
        notifica.innerText =
            'Reset eseguito. Il dispositivo si sta riavviando...';
      } catch (e) {
        notifica.className = 'error';
        notifica.innerText = 'Errore durante il reset di fabbrica.';
        btnResetEsp.disabled = false;
      }
    });
  }

  // Funzione per formattare tempo trascorso
  function formattaTempoTrascorso(elapsed) {
    if (elapsed === 0) return 'ora';
    if (elapsed < 60) {
      return `${elapsed} s fa`;
    } else if (elapsed < 3600) {
      const mins = Math.floor(elapsed / 60);
      return `${mins} min fa`;
    } else {
      const hours = Math.floor(elapsed / 3600);
      const mins = Math.floor((elapsed % 3600) / 60);
      return `${hours} h ${mins} min fa`;
    }
  }

  async function caricaStatoSistema() {
    try {
      const res = await fetch('/status');
      if (res.ok) {
        const data = await res.json();
        aggiornaEtichette(data);
      } else {
        throw new Error('Errore di comunicazione');
      }
    } catch (e) {
      lblSsid.textContent = 'Rete: Disconnesso';
      lblNetworkStatus.textContent = 'Stato Rete: Non connessa';
      lblLastSync.textContent = 'Ultima Sincronizzazione: ---';
      lblFirmware.textContent = `Firmware: ${FIRMWARE_VERSION}`;
    }
  }

  caricaStatoSistema();
  setInterval(caricaStatoSistema, 3000);
});