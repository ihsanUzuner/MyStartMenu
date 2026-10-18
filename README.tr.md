# BetterStartMenu

[English](README.md) · Türkçe

Windows 11 için, Win tuşuna bastığınızda ya da Başlat düğmesine tıkladığınızda Windows'un kendi menüsünün yerine
açılan kişisel bir Başlat menüsü. Direct2D ve Windows.UI.Composition ile düz C++ ile yazıldı: .NET yok, XAML yok,
kurulum programı yok, yaklaşık 30 MB bellek.

Arayüz Türkçedir.

<!-- Ekran görüntüleri: görüntüleri docs/ klasörüne ekleyip buradan bağlantı verin, ör. ![Menü](docs/menu.png) -->

## Özellikler

- **Gruplar halinde sabitlenmiş uygulamalar.** Uygulamaları sürükleyerek sıralarını değiştirin ya da yeni bir grup
  başlatın; bir grubu taşımak için tutamacından sürükleyin. Grupların adı olabilir, çerçeveyle ya da altı çizili
  gösterilebilir ve dört sütuna kadar dizilebilir.
- Uygulamaların yanında, klasörlerin kendi simgeleriyle bir **klasörler sütunu**; herhangi bir klasörü ya da kabuk
  konumunu (Bu bilgisayar, Ağ, Denetim Masası) ekleyin.
- Harf başlıklarıyla A–Z ya da en son yüklenen önce sıralanan **tüm uygulamalar listesi**; bir uygulamayı sabitlemek
  ya da sabitlemesini kaldırmak için tıklayın.
- Alt çubuktaki kutudan, yüklü uygulamalarda **arama**.
- Ayarlanabilir saydamlık ve Windows'un vurgu rengi de dahil bir dizi ton rengiyle **akrilik arka plan**.
- **Ayarlanabilir düzen:** menü genişliği ve yüksekliği, uygulama boyutu ve aralığı, simge boyutları, uygulama
  adları açık ya da kapalı.
- Ok tuşlarıyla klavyeden gezinme ve Windows'unkiler gibi güç ve hesap menüleri.

Bütün ayarlar ayar panelinde (hesap menüsündeki "Menü ayarları") bulunur ve `%AppData%\MyStartMenu\config.json`
dosyasına kaydedilir.

## Kurulum

1. `MyStartMenu.exe`'yi [Releases](../../releases) sayfasından indirin.
2. Kalıcı olarak duracağı, kendine ait bir klasöre koyun, örneğin `C:\Programlar\MyStartMenu\`. (Hata günlüğünü
   kendi yanına yazdığı için yazma izniniz olan bir klasör seçin.)
3. Çalıştırın. Menü açılır ve bundan sonra Windows ile birlikte başlar.

Windows 11, x64 gerektirir. Başka hiçbir şey kurmanız gerekmez.

### Bilmekte fayda var

- **Exe'yi taşıdınız mı?** Windows ile başlatma görevi exe'nin nerede olduğunu hatırlar. Tepsi simgesine sağ
  tıklayın ve "Başlangıçta çalıştır"ı kapatıp yeniden açın.
- **SmartScreen uyarısı.** Exe imzalı olmadığı için Windows ilk seferde tanınmayan bir uygulama uyarısı verebilir.
  "Ek bilgi"yi, ardından "Yine de çalıştır"ı seçin.
- **Virüs programı.** Menü, Win tuşunu ve Başlat düğmesini yakalamak için sistem genelinde bir klavye ve fare
  kancası kullanır; bazı virüs programları buna ikinci kez bakar.
- **İlk çalıştırma.** Menü, mevcut Windows Başlat sabitlerinizi bir kez kopyalar; bundan sonra sabitleri kendisinindir
  ve Windows'unkilerle eşitlenmez.
- Tepsi simgesinin menüsünden ("Win tuşu ve Başlat düğmesi bu menüyü açsın") **bir süreliğine kapatın**; o zaman
  Windows'un kendi Başlat menüsü her zamanki gibi çalışır.

### Bilinen sınırlamalar

- Yönetici olarak çalışan bir programın menüsü açıkken (örneğin bazı tepsi simgelerinin sağ tık menüleri), Başlat
  düğmesine tıklamak Windows'un kendi menüsünü açar: Windows, normal bir programın bu tıklamaları görmesine izin
  vermez. Win tuşu yine çalışır.
- Kabuğun kendi panelleri (arama, bildirim merkezi, hızlı ayarlar, atlama listeleri) menü açılırken kapatılır;
  birkaçının aynı anda açık olduğu nadir durumlarda biri açık kalabilir.

## Derleme

Visual Studio 2026 Build Tools (MSVC, C++ iş yükü) ve Windows SDK 10.0.26100 gerekir.

```
build.cmd
```

`bin\MyStartMenu.exe`'yi derler. Proje dosyası yoktur; `build.cmd`, Build Tools'un varsayılan kurulum klasöründeki
`vcvars64.bat`'tan sonra `cl`'yi doğrudan çağırır (sizinki başka bir yerdeyse `build.cmd`'deki yolu düzenleyin). C++
çalışma kitaplıkları statik olarak bağlanır, bu yüzden exe tek başına çalışır.

`MyStartMenu.exe --render <klasör>`, hiçbir şey göstermeden menünün görünümlerini PNG dosyalarına çizer; düzen
değişikliklerini kontrol etmek için kullanışlıdır.

## Lisans

[MIT](LICENSE)
