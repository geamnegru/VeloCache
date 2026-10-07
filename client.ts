import * as net from 'net';

export class VeloClient {
  private readonly port: number;
  private readonly host: string;

  constructor(port: number = 6379, host: string = '127.0.0.1') {
    this.port = port;
    this.host = host;
  }

  private trimite(comanda: string[]): Promise<string> {
    return new Promise<string>((resolve, reject) => {
      const socket = new net.Socket();
      const maxBulkBytes = 1024 * 1024;
      const parti = [`*${comanda.length}\r\n`];
      let raspuns = Buffer.alloc(0);
      let terminat = false;

      for (const argument of comanda) {
        parti.push(`$${Buffer.byteLength(argument, 'utf8')}\r\n`, argument, '\r\n');
      }

      const esueaza = (error: Error): void => {
        if (terminat) return;
        terminat = true;
        reject(error);
        socket.destroy();
      };

      const rezolva = (valoare: string): void => {
        if (terminat) return;
        terminat = true;
        resolve(valoare);
        socket.destroy();
      };

      socket.on('data', (data: Buffer) => {
        if (terminat) return;
        if (raspuns.length + data.length > maxBulkBytes + 64) {
          esueaza(new Error('Răspunsul RESP depășește limita de 1 MiB.'));
          return;
        }

        raspuns = Buffer.concat([
          new Uint8Array(raspuns.buffer, raspuns.byteOffset, raspuns.byteLength),
          new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
        ]);
        const tip = raspuns[0];
        if (tip !== 43 && tip !== 45 && tip !== 36) {
          esueaza(new Error('Tip de răspuns RESP neacceptat.'));
          return;
        }

        let sfarsitLinie = -1;
        for (let index = 1; index < raspuns.length; index++) {
          if (raspuns[index] === 10) {
            esueaza(new Error('Răspuns RESP invalid: terminator CRLF obligatoriu.'));
            return;
          }
          if (raspuns[index] === 13) {
            if (index + 1 === raspuns.length) return;
            if (raspuns[index + 1] !== 10) {
              esueaza(new Error('Răspuns RESP invalid: terminator CRLF obligatoriu.'));
              return;
            }
            sfarsitLinie = index;
            break;
          }
          if (tip === 36 && index > 16) {
            esueaza(new Error('Lungime RESP invalidă.'));
            return;
          }
        }
        if (sfarsitLinie === -1) return;

        const text = raspuns.subarray(1, sfarsitLinie).toString('utf8');
        const inceput = sfarsitLinie + 2;
        if (tip === 43 || tip === 45) {
          if (raspuns.length !== inceput || sfarsitLinie - 1 > maxBulkBytes) {
            esueaza(new Error('Răspuns RESP invalid.'));
          } else if (tip === 45) {
            esueaza(new Error(text));
          } else {
            rezolva(text);
          }
          return;
        }

        if (text === '-1') {
          if (raspuns.length !== inceput) {
            esueaza(new Error('Răspuns RESP invalid.'));
          } else {
            rezolva('(nil)');
          }
          return;
        }
        if (!/^(0|[1-9][0-9]*)$/.test(text)) {
          esueaza(new Error('Lungime RESP invalidă.'));
          return;
        }

        const lungime = Number(text);
        if (!Number.isSafeInteger(lungime) || lungime > maxBulkBytes) {
          esueaza(new Error('Răspunsul RESP depășește limita de 1 MiB.'));
          return;
        }

        const sfarsit = inceput + lungime;
        if (raspuns.length < sfarsit + 2) return;
        if (raspuns.length !== sfarsit + 2 || raspuns[sfarsit] !== 13 || raspuns[sfarsit + 1] !== 10) {
          esueaza(new Error('Răspuns RESP bulk invalid.'));
          return;
        }
        rezolva(raspuns.subarray(inceput, sfarsit).toString('utf8'));
      });

      socket.once('error', esueaza);

      socket.once('end', () => {
        esueaza(new Error('Conexiunea s-a închis fără un răspuns RESP complet.'));
      });

      socket.once('close', () => {
        esueaza(new Error('Conexiunea s-a închis fără un răspuns RESP complet.'));
      });

      socket.setTimeout(5000, () => {
        esueaza(new Error('Conexiunea a depășit timpul de așteptare de 5 secunde.'));
      });

      socket.connect(this.port, this.host, () => {
        socket.write(parti.join(''), 'utf8');
      });
    });
  }

  async ping(): Promise<string> {
    return this.trimite(['PING']);
  }

  async set(key: string, value: string, ttlSeconds?: number): Promise<string> {
    const comanda = ['SET', key, value];
    if (ttlSeconds !== undefined) comanda.push('EX', String(ttlSeconds));
    return this.trimite(comanda);
  }

  async get(key: string): Promise<string> {
    return this.trimite(['GET', key]);
  }
}

async function main(): Promise<void> {
  const client = new VeloClient();

  console.log('PING:', await client.ping());
  console.log('SET:', await client.set('mesaj', 'Salut VeloCache'));
  console.log('GET:', await client.get('mesaj'));
  console.log('SET EX:', await client.set('temporar', 'Expir în 1 secundă', 1));
  console.log('GET înainte de expirare:', await client.get('temporar'));

  await new Promise<void>((resolve) => setTimeout(resolve, 2500));

  console.log('GET după expirare:', await client.get('temporar'));
}

if (process.argv[1]) {
  const cale = process.argv[1].startsWith('/') ? process.argv[1] : `${process.cwd()}/${process.argv[1]}`;
  const url = new URL(`file://${cale.split('/').map(encodeURIComponent).join('/')}`);
  if (import.meta.url === url.href) {
    main().catch((error: unknown) => {
      console.error(error);
      process.exitCode = 1;
    });
  }
}
