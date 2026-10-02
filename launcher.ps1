Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'toxiccity.exe'

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Spider-Man: Toxic City — Launcher'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(600, 520)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

function Add-Label($text, $x, $y, $width = 150) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $text
    $label.Location = New-Object System.Drawing.Point($x, $y)
    $label.Size = New-Object System.Drawing.Size($width, 24)
    $form.Controls.Add($label)
    return $label
}
function Add-TextBox($value, $x, $y, $width = 330) {
    $box = New-Object System.Windows.Forms.TextBox
    $box.Text = $value
    $box.Location = New-Object System.Drawing.Point($x, $y)
    $box.Size = New-Object System.Drawing.Size($width, 25)
    $form.Controls.Add($box)
    return $box
}
function Add-BrowseButton($text, $x, $y, [scriptblock]$action) {
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $text
    $button.Location = New-Object System.Drawing.Point($x, $y)
    $button.Size = New-Object System.Drawing.Size(82, 26)
    $button.Add_Click($action)
    $form.Controls.Add($button)
    return $button
}
function Browse-Folder($box, $description) {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = $description
    $dialog.SelectedPath = $box.Text
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $box.Text = $dialog.SelectedPath }
}

Add-Label 'Game executable' 20 20
$exeBox = Add-TextBox $exe 175 18 330
Add-BrowseButton 'Browse…' 512 17 { $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Filter = 'Executable (*.exe)|*.exe'; $d.InitialDirectory = $root; if ($d.ShowDialog() -eq 'OK') { $exeBox.Text = $d.FileName } } | Out-Null

Add-Label 'Resources folder' 20 60
$resBox = Add-TextBox (Join-Path $root 'res') 175 58 330
Add-BrowseButton 'Browse…' 512 57 { Browse-Folder $resBox 'Choose the extracted game resources folder' } | Out-Null

Add-Label 'Save folder' 20 100
$saveBox = Add-TextBox (Join-Path $root 'save') 175 98 330
Add-BrowseButton 'Browse…' 512 97 { Browse-Folder $saveBox 'Choose where game saves are stored' } | Out-Null

Add-Label 'Window scale' 20 145
$scaleBox = New-Object System.Windows.Forms.ComboBox
$scaleBox.Location = New-Object System.Drawing.Point(175, 142)
$scaleBox.Size = New-Object System.Drawing.Size(100, 25)
$scaleBox.DropDownStyle = 'DropDownList'
1..8 | ForEach-Object { [void]$scaleBox.Items.Add($_) }
$scaleBox.SelectedItem = 4
$form.Controls.Add($scaleBox)
Add-Label 'integer enlargement of the 240×320 game image' 290 145 280 | Out-Null

Add-Label 'FPS target' 20 185
$fpsBox = New-Object System.Windows.Forms.ComboBox
$fpsBox.Location = New-Object System.Drawing.Point(175, 182)
$fpsBox.Size = New-Object System.Drawing.Size(100, 25)
$fpsBox.DropDownStyle = 'DropDownList'
@(15, 20, 30, 40, 50, 60) | ForEach-Object { [void]$fpsBox.Items.Add($_) }
$fpsBox.SelectedItem = 60
Add-Label 'frame pacing target (maximum 60)' 290 185 285 | Out-Null
$form.Controls.Add($fpsBox)

$wideCheck = New-Object System.Windows.Forms.CheckBox
$wideCheck.Text = 'Widescreen window (portrait game centered with side bars)'
$wideCheck.Location = New-Object System.Drawing.Point(20, 212); $wideCheck.Size = New-Object System.Drawing.Size(480, 24); $form.Controls.Add($wideCheck)
$wideCheck.Add_CheckedChanged({ $scaleBox.Enabled = -not $wideCheck.Checked })

$separator = New-Object System.Windows.Forms.Label
$separator.BorderStyle = 'Fixed3D'; $separator.Location = New-Object System.Drawing.Point(20, 242); $separator.Size = New-Object System.Drawing.Size(555, 2); $form.Controls.Add($separator)

$dumpCheck = New-Object System.Windows.Forms.CheckBox
$dumpCheck.Text = 'Export decoded textures while the game runs'
$dumpCheck.Location = New-Object System.Drawing.Point(20, 260); $dumpCheck.Size = New-Object System.Drawing.Size(360, 24); $form.Controls.Add($dumpCheck)
Add-Label 'Export folder' 20 292
$dumpBox = Add-TextBox (Join-Path $root 'textures_original') 175 290 330
Add-BrowseButton 'Browse…' 512 289 { Browse-Folder $dumpBox 'Choose a texture export folder' } | Out-Null

$overrideCheck = New-Object System.Windows.Forms.CheckBox
$overrideCheck.Text = 'Load edited PNG texture overrides'
$overrideCheck.Location = New-Object System.Drawing.Point(20, 330); $overrideCheck.Size = New-Object System.Drawing.Size(360, 24); $form.Controls.Add($overrideCheck)
Add-Label 'Overrides folder' 20 362
$overrideBox = Add-TextBox (Join-Path $root 'textures_mod') 175 360 330
Add-BrowseButton 'Browse…' 512 359 { Browse-Folder $overrideBox 'Choose the edited texture PNG folder' } | Out-Null

$help = New-Object System.Windows.Forms.LinkLabel
$help.Text = 'Texture workflow instructions'
$help.Location = New-Object System.Drawing.Point(20, 404); $help.Size = New-Object System.Drawing.Size(250, 24)
$help.Add_LinkClicked({ Start-Process (Join-Path $root 'TEXTURES.md') })
$form.Controls.Add($help)
$logLink = New-Object System.Windows.Forms.LinkLabel
$logLink.Text = 'Open texture override log'
$logLink.Location = New-Object System.Drawing.Point(280, 404); $logLink.Size = New-Object System.Drawing.Size(220, 24)
$logLink.Add_LinkClicked({ $p = Join-Path $root 'texture_override.log'; if (Test-Path -LiteralPath $p) { Start-Process notepad.exe $p } else { [System.Windows.Forms.MessageBox]::Show('The log is created when the game runs with texture overrides enabled.') } })
$form.Controls.Add($logLink)

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(20, 442); $status.Size = New-Object System.Drawing.Size(360, 30); $status.Text = 'Live FPS and frame time metrics appear in the game title.'
$form.Controls.Add($status)

$launch = New-Object System.Windows.Forms.Button
$launch.Text = 'Launch game'
$launch.Location = New-Object System.Drawing.Point(420, 432); $launch.Size = New-Object System.Drawing.Size(155, 42)
$launch.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
$launch.Add_Click({
    if (-not (Test-Path -LiteralPath $exeBox.Text -PathType Leaf)) { [System.Windows.Forms.MessageBox]::Show('Choose a valid game executable.'); return }
    if (-not (Test-Path -LiteralPath $resBox.Text -PathType Container)) { [System.Windows.Forms.MessageBox]::Show('Choose a valid resources folder.'); return }
    if (-not (Test-Path -LiteralPath $saveBox.Text -PathType Container)) { New-Item -ItemType Directory -Force -Path $saveBox.Text | Out-Null }
    if ($dumpCheck.Checked -and -not (Test-Path -LiteralPath $dumpBox.Text -PathType Container)) { New-Item -ItemType Directory -Force -Path $dumpBox.Text | Out-Null }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exeBox.Text
    $psi.WorkingDirectory = Split-Path -Parent $exeBox.Text
    $psi.Arguments = '--res "{0}" --save "{1}" --scale {2} --fps {3}' -f $resBox.Text, $saveBox.Text, $scaleBox.SelectedItem, $fpsBox.SelectedItem
    if ($wideCheck.Checked) { $psi.Arguments += ' --wide' }
    $psi.UseShellExecute = $false
    $sdlDirs = @((Split-Path -Parent $exeBox.Text), 'C:\msys64\ucrt64\bin', 'C:\msys64\mingw64\bin')
    foreach ($dir in $sdlDirs) {
        if (Test-Path -LiteralPath (Join-Path $dir 'SDL2.dll')) {
            $psi.EnvironmentVariables['PATH'] = $dir + ';' + $psi.EnvironmentVariables['PATH']
            break
        }
    }
    if ($dumpCheck.Checked) { $psi.EnvironmentVariables['TOXICCITY_DUMP_TEXTURES'] = $dumpBox.Text } else { $psi.EnvironmentVariables.Remove('TOXICCITY_DUMP_TEXTURES') }
    if ($overrideCheck.Checked) {
        if (-not (Test-Path -LiteralPath $overrideBox.Text -PathType Container)) { [System.Windows.Forms.MessageBox]::Show('Choose an existing texture overrides folder.'); return }
        $psi.EnvironmentVariables['TOXICCITY_TEXTURE_OVERRIDES'] = $overrideBox.Text
    } else { $psi.EnvironmentVariables.Remove('TOXICCITY_TEXTURE_OVERRIDES') }
    try { [void][System.Diagnostics.Process]::Start($psi); $status.Text = 'Game launched.' }
    catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Launch failed') }
})
$form.Controls.Add($launch)

[void]$form.ShowDialog()
