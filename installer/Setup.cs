// ตัวติดตั้งปลั๊กอิน Honda ECU สำหรับ TunerPro RT v6
//
// ปลั๊กอินเป็น DLL ไฟล์เดียวที่ฝังอยู่ในตัวติดตั้งนี้ ติดตั้งคือคัดลอกไปไว้ที่
// Documents\TunerPro Files\Plugins ซึ่งเป็นโฟลเดอร์ที่ TunerPro โหลดปลั๊กอินเอง
// ไม่ต้องใช้สิทธิ์แอดมิน ไม่แก้ไฟล์ของ TunerPro ถอดออกคือลบไฟล์ที่ติดตั้งไว้
//
// ส่วนนี้ไม่ได้ติดตั้งไดรเวอร์ FTDI หรือ J2534 ให้ ต้องลงจากผู้ผลิตสายเอง

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

[assembly: AssemblyTitle("Honda ECU plug-in setup for TunerPro")]
[assembly: AssemblyProduct("TunerPro-HondaECU")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace TunerProHondaECU
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            // โหมดไม่มีหน้าต่างสำหรับสคริปต์: /install หรือ /uninstall
            if (args.Length == 1 && args[0] == "/install") return Core.Install();
            if (args.Length == 1 && args[0] == "/uninstall") return Core.Uninstall();

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm());
            return 0;
        }
    }

    static class Core
    {
        const string PluginFile = "HondaECU.dll";
        const string LogFile = "HondaECU.log";

        public static string PluginDir
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "TunerPro Files", "Plugins");
            }
        }

        public static string PluginPath
        {
            get { return Path.Combine(PluginDir, PluginFile); }
        }

        public static bool IsInstalled
        {
            get { return File.Exists(PluginPath); }
        }

        public static bool IsTunerProRunning
        {
            get { return Process.GetProcessesByName("TunerPro").Length > 0; }
        }

        // ที่อยู่มาตรฐานของ TunerPro RT v6 ใช้แค่เตือนถ้าไม่พบ ไม่ได้ห้ามติดตั้ง
        public static bool TunerProFound
        {
            get
            {
                string pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
                return File.Exists(Path.Combine(pf86, "TunerPro RT v6", "TunerPro.exe"))
                    || Directory.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "TunerPro Files"));
            }
        }

        static byte[] EmbeddedPlugin()
        {
            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(PluginFile))
            using (MemoryStream m = new MemoryStream())
            {
                s.CopyTo(m);
                return m.ToArray();
            }
        }

        // เขียนลงไฟล์ชั่วคราวก่อนแล้วค่อยแทนที่ ไฟล์เดิมจะได้ไม่เสียถ้าเขียนไม่ครบ
        public static int Install()
        {
            if (IsTunerProRunning) return 2;
            try
            {
                Directory.CreateDirectory(PluginDir);
                string temp = PluginPath + ".new";
                File.WriteAllBytes(temp, EmbeddedPlugin());
                if (File.Exists(PluginPath)) File.Delete(PluginPath);
                File.Move(temp, PluginPath);
                return 0;
            }
            catch (Exception)
            {
                return 1;
            }
        }

        public static int Uninstall()
        {
            if (IsTunerProRunning) return 2;
            try
            {
                if (File.Exists(PluginPath)) File.Delete(PluginPath);
                string log = Path.Combine(PluginDir, LogFile);
                if (File.Exists(log)) File.Delete(log);
                return 0;
            }
            catch (Exception)
            {
                return 1;
            }
        }
    }

    static class Lang
    {
        static readonly bool Thai = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "th"
            || System.Globalization.CultureInfo.CurrentCulture.TwoLetterISOLanguageName == "th";

        public static string T(string thai, string english)
        {
            return Thai ? thai : english;
        }
    }

    class SetupForm : Form
    {
        readonly Label lblStatus = new Label();
        readonly Label lblInfo = new Label();
        readonly Button btnInstall = new Button();
        readonly Button btnUninstall = new Button();
        readonly Timer timer = new Timer();

        public SetupForm()
        {
            Text = Lang.T("ติดตั้งปลั๊กอิน Honda ECU", "Honda ECU plug-in setup");
            Font = new Font("Tahoma", 9.75f);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(ScaleX(480), ScaleX(250));
            Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);

            Label title = new Label();
            title.Text = Lang.T("Honda ECU สำหรับ TunerPro RT v6", "Honda ECU for TunerPro RT v6");
            title.Font = new Font("Tahoma", 13f, FontStyle.Bold);
            title.SetBounds(ScaleX(20), ScaleX(16), ScaleX(440), ScaleX(28));
            Controls.Add(title);

            Label detail = new Label();
            detail.Text = Lang.T(
                "เพิ่มเมนู Honda ECU ใน TunerPro สำหรับอ่านและเขียนไฟล์ ดู EEPROM และรหัสปัญหา\r\nใช้ได้กับสาย FTDI, Scanmatik SM2 และ Tactrix OpenPort 2.0",
                "Adds a Honda ECU menu to TunerPro to read and write the ROM, view the EEPROM and trouble codes.\r\nWorks with FTDI, Scanmatik SM2 and Tactrix OpenPort 2.0 cables.");
            detail.SetBounds(ScaleX(20), ScaleX(50), ScaleX(440), ScaleX(64));
            Controls.Add(detail);

            lblStatus.Font = new Font("Tahoma", 9.75f, FontStyle.Bold);
            lblStatus.SetBounds(ScaleX(20), ScaleX(120), ScaleX(440), ScaleX(22));
            Controls.Add(lblStatus);

            lblInfo.ForeColor = Color.FromArgb(160, 60, 0);
            lblInfo.SetBounds(ScaleX(20), ScaleX(144), ScaleX(440), ScaleX(40));
            Controls.Add(lblInfo);

            btnInstall.SetBounds(ScaleX(20), ScaleX(196), ScaleX(216), ScaleX(38));
            btnInstall.Click += delegate { Run(true); };
            Controls.Add(btnInstall);

            btnUninstall.Text = Lang.T("ถอดออก", "Uninstall");
            btnUninstall.SetBounds(ScaleX(244), ScaleX(196), ScaleX(216), ScaleX(38));
            btnUninstall.Click += delegate { Run(false); };
            Controls.Add(btnUninstall);

            timer.Interval = 1000;
            timer.Tick += delegate { RefreshState(); };
            timer.Start();
            RefreshState();
        }

        int ScaleX(int value)
        {
            using (Graphics g = CreateGraphics())
            {
                return (int)(value * g.DpiX / 96f);
            }
        }

        void RefreshState()
        {
            bool installed = Core.IsInstalled;
            bool running = Core.IsTunerProRunning;
            lblStatus.Text = installed ? Lang.T("สถานะ: ติดตั้งแล้ว", "Status: installed") : Lang.T("สถานะ: ยังไม่ได้ติดตั้ง", "Status: not installed");
            lblStatus.ForeColor = installed ? Color.ForestGreen : SystemColors.ControlText;
            btnInstall.Text = installed ? Lang.T("ติดตั้งทับ", "Reinstall") : Lang.T("ติดตั้ง", "Install");
            btnInstall.Enabled = !running;
            btnUninstall.Enabled = installed && !running;

            if (running) lblInfo.Text = Lang.T("TunerPro เปิดอยู่ ปิดโปรแกรมก่อนจึงจะติดตั้งหรือถอดออกได้", "TunerPro is running. Close it before installing or uninstalling.");
            else if (!Core.TunerProFound) lblInfo.Text = Lang.T("ไม่พบ TunerPro RT v6 ในเครื่อง ติดตั้งปลั๊กอินไว้ก่อนได้ แต่ต้องลง TunerPro จึงจะใช้งานได้", "TunerPro RT v6 was not found. You can install now, but the plug-in needs TunerPro to run.");
            else lblInfo.Text = "";
        }

        void Run(bool install)
        {
            if (!install)
            {
                DialogResult answer = MessageBox.Show(this, Lang.T("ถอดปลั๊กอิน Honda ECU ออกจาก TunerPro?", "Remove the Honda ECU plug-in from TunerPro?"), Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2);
                if (answer != DialogResult.Yes) return;
            }

            int result = install ? Core.Install() : Core.Uninstall();
            RefreshState();
            if (result == 0)
            {
                string done = install
                    ? Lang.T("ติดตั้งเรียบร้อย เปิด TunerPro แล้วดูเมนู Honda ECU", "Installed. Open TunerPro and look for the Honda ECU menu.")
                    : Lang.T("ถอดออกเรียบร้อย", "Uninstalled.");
                MessageBox.Show(this, done, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else if (result == 2)
            {
                MessageBox.Show(this, Lang.T("TunerPro เปิดอยู่ ปิดโปรแกรมก่อนแล้วลองใหม่", "TunerPro is running. Close it and try again."), Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            else
            {
                MessageBox.Show(this, Lang.T("ทำรายการไม่สำเร็จ เขียนไฟล์ในโฟลเดอร์นี้ไม่ได้:\r\n", "That did not work. Cannot write to this folder:\r\n") + Core.PluginDir, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }
}
