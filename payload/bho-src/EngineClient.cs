using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace IEBridgeBho
{
    internal delegate void LineReceivedHandler(string line);

    // Talks to the engine bridge over loopback TCP. Events fire on a worker thread.
    internal sealed class EngineClient
    {
        private readonly int port;
        private readonly string token;
        private NetworkStream stream;
        private Thread thread;
        private volatile bool closing;
        private readonly object sendLock = new object();

        public event LineReceivedHandler LineReceived;
        public event EventHandler ConnectFailed;
        public event EventHandler Disconnected;

        public EngineClient(int port, string token)
        {
            this.port = port;
            this.token = token;
        }

        // True when something is already listening on the engine's port.
        public static bool Probe(int port)
        {
            try
            {
                TcpClient c = new TcpClient();
                IAsyncResult ar = c.BeginConnect("127.0.0.1", port, null, null);
                bool done = ar.AsyncWaitHandle.WaitOne(400, false);
                bool ok = done && c.Connected;
                try { if (done) c.EndConnect(ar); } catch (Exception) { ok = false; }
                c.Close();
                return ok;
            }
            catch (Exception) { return false; }
        }

        public void Start()
        {
            thread = new Thread(new ThreadStart(Run));
            thread.IsBackground = true;
            thread.Start();
        }

        public void Send(string line)
        {
            lock (sendLock)
            {
                if (stream == null) return;
                try
                {
                    byte[] data = Encoding.ASCII.GetBytes(line.Replace("\r", " ").Replace("\n", " ") + "\r\n");
                    stream.Write(data, 0, data.Length);
                    stream.Flush();
                }
                catch (IOException) { }
                catch (ObjectDisposedException) { }
            }
        }

        public void Close()
        {
            closing = true;
            lock (sendLock)
            {
                if (stream != null)
                {
                    try { stream.Close(); } catch (IOException) { }
                    stream = null;
                }
            }
        }

        private void Run()
        {
            NetworkStream s = null;
            DateTime deadline = DateTime.Now.AddSeconds(120);
            while (!closing && DateTime.Now < deadline)
            {
                try
                {
                    TcpClient c = new TcpClient();
                    c.Connect("127.0.0.1", port);
                    s = c.GetStream();
                    break;
                }
                catch (SocketException) { Thread.Sleep(500); }
            }
            if (s == null)
            {
                if (!closing) Raise(ConnectFailed);
                return;
            }

            lock (sendLock) { stream = s; }
            Send("AUTH " + token);

            try
            {
                StreamReader reader = new StreamReader(s, Encoding.ASCII);
                string line;
                while (!closing && (line = reader.ReadLine()) != null)
                {
                    LineReceivedHandler h = LineReceived;
                    if (h != null && line.Length > 0) h(line);
                }
            }
            catch (IOException) { }
            catch (ObjectDisposedException) { }

            if (!closing) Raise(Disconnected);
        }

        private void Raise(EventHandler h)
        {
            if (h != null) h(this, EventArgs.Empty);
        }
    }
}
