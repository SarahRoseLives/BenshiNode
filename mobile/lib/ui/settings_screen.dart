import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:provider/provider.dart';

import '../core/node_controller.dart';
import '../services/bluetooth_classic.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  late final TextEditingController _callsign;
  late final TextEditingController _mac;
  late final TextEditingController _channel;
  late final TextEditingController _statusPoll;
  late final TextEditingController _voxThreshold;
  late final TextEditingController _ttsRate;
  late final TextEditingController _linkServer;
  late final TextEditingController _linkKey;
  late bool _ttsEnabled;
  late bool _linkAuto;
  late bool _parrotAuto;
  late String _squelchMode;
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    final config = context.read<NodeController>().config!;
    _callsign = TextEditingController(text: config.callsign);
    _mac = TextEditingController(text: config.mac);
    final channel = config.getString('RADIO', 'CHANNEL');
    _channel = TextEditingController(text: channel);
    _statusPoll = TextEditingController(
      text: config.getInt('RADIO', 'STATUS_POLL_MS', 500).toString(),
    );
    _voxThreshold = TextEditingController(
      text: config.getInt('NODE', 'VOX_THRESHOLD_DB', -38).toString(),
    );
    _ttsRate = TextEditingController(
      text: config.getDouble('PIPER', 'TTS_RATE', 0.4).toStringAsFixed(2),
    );
    _linkServer = TextEditingController(text: config.linkServer);
    _linkKey = TextEditingController(text: config.linkKey);
    _ttsEnabled = config.ttsEnabled;
    _linkAuto = config.linkEnabled;
    _parrotAuto =
        config.getString('NODE', 'PARROT_MODE', 'dtmf').toLowerCase() == 'auto';
    _squelchMode =
        config.getString('NODE', 'SQUELCH_MODE', 'auto').toLowerCase();
  }

  @override
  void dispose() {
    _callsign.dispose();
    _mac.dispose();
    _channel.dispose();
    _statusPoll.dispose();
    _voxThreshold.dispose();
    _ttsRate.dispose();
    _linkServer.dispose();
    _linkKey.dispose();
    super.dispose();
  }

  Future<void> _pickRadio() async {
    final controller = context.read<NodeController>();
    try {
      await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
      ].request();
      final devices = await BluetoothClassic.instance.getPairedDevices();
      if (!mounted) {
        return;
      }
      if (devices.isEmpty) {
        await showDialog<void>(
          context: context,
          builder: (context) => AlertDialog(
            title: const Text('No paired radios'),
            content: const Text(
              'Pair the radio in Android Bluetooth settings, then try again.',
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(context),
                child: const Text('OK'),
              ),
            ],
          ),
        );
        return;
      }
      final selected = await showDialog<BtDeviceInfo>(
        context: context,
        builder: (context) => SimpleDialog(
          title: const Text('Select radio'),
          children: [
            for (final device in devices)
              SimpleDialogOption(
                onPressed: () => Navigator.pop(context, device),
                child: ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: const Icon(Icons.bluetooth),
                  title: Text(device.name),
                  subtitle: Text(device.address),
                  trailing: device.serviceUuids.isEmpty
                      ? null
                      : const Icon(Icons.radio, size: 18),
                ),
              ),
          ],
        ),
      );
      if (selected != null && mounted) {
        setState(() {
          _mac.text = selected.address;
        });
      }
    } catch (error) {
      controller.logger.warning('Could not list Bluetooth devices: $error');
    }
  }

  Future<void> _saveAndRestart() async {
    if (_saving) {
      return;
    }
    setState(() => _saving = true);
    final controller = context.read<NodeController>();
    final config = controller.config!;
    config.set('NODE', 'CALLSIGN', _callsign.text.trim());
    config.set('RADIO', 'MAC', _mac.text.trim().toUpperCase());
    config.set('RADIO', 'CHANNEL', _channel.text.trim());
    config.set(
      'RADIO',
      'STATUS_POLL_MS',
      int.tryParse(_statusPoll.text.trim())?.toString() ?? '500',
    );
    config.set(
      'NODE',
      'VOX_THRESHOLD_DB',
      int.tryParse(_voxThreshold.text.trim())?.toString() ?? '-38',
    );
    config.set('NODE', 'PARROT_MODE', _parrotAuto ? 'auto' : 'dtmf');
    config.set('NODE', 'SQUELCH_MODE', _squelchMode);
    config.set('PIPER', 'ENABLED', _ttsEnabled ? '1' : '0');
    config.set(
      'PIPER',
      'TTS_RATE',
      double.tryParse(_ttsRate.text.trim())?.toStringAsFixed(2) ?? '0.4',
    );
    config.set('LINK', 'SERVER', _linkServer.text.trim());
    config.set('LINK', 'KEY', _linkKey.text.trim());
    config.set('LINK', 'ENABLED', _linkAuto ? '1' : '0');
    await config.save();
    if (!mounted) {
      return;
    }
    final messenger = ScaffoldMessenger.of(context);
    Navigator.of(context).pop();
    await controller.reload();
    messenger.showSnackBar(
      const SnackBar(content: Text('Settings saved; node restarted')),
    );
    setState(() => _saving = false);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _section('Node'),
          TextField(
            controller: _callsign,
            textCapitalization: TextCapitalization.characters,
            decoration: const InputDecoration(labelText: 'Callsign'),
          ),
          const SizedBox(height: 12),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Spoken prompts (Android TTS)'),
            subtitle: const Text('Synthesizes node prompts and transmits them.'),
            value: _ttsEnabled,
            onChanged: (value) => setState(() => _ttsEnabled = value),
          ),
          const SizedBox(height: 4),
          TextField(
            controller: _ttsRate,
            keyboardType: const TextInputType.numberWithOptions(decimal: true),
            decoration: const InputDecoration(
              labelText: 'Voice speed',
              helperText: '0.5 = normal; lower is slower',
            ),
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Parrot always on'),
            subtitle: const Text('Off = toggle with 1#.'),
            value: _parrotAuto,
            onChanged: (value) => setState(() => _parrotAuto = value),
          ),
          const SizedBox(height: 4),
          DropdownButtonFormField<String>(
            initialValue: _squelchMode,
            decoration: const InputDecoration(
              labelText: 'Squelch mode',
              helperText: 'Radio is recommended on Android',
            ),
            items: const [
              DropdownMenuItem(value: 'radio', child: Text('Radio')),
              DropdownMenuItem(value: 'vox', child: Text('VOX')),
              DropdownMenuItem(value: 'auto', child: Text('Radio + VOX')),
            ],
            onChanged: (value) =>
                setState(() => _squelchMode = value ?? 'auto'),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _voxThreshold,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'VOX threshold (dB)',
              helperText: 'Typical range -50 to -20 dB',
            ),
          ),
          const SizedBox(height: 18),
          _section('Radio'),
          Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Expanded(
                child: TextField(
                  controller: _mac,
                  textCapitalization: TextCapitalization.characters,
                  decoration: const InputDecoration(
                    labelText: 'Bluetooth MAC',
                    hintText: 'AA:BB:CC:DD:EE:FF',
                  ),
                ),
              ),
              const SizedBox(width: 8),
              FilledButton.tonalIcon(
                onPressed: _pickRadio,
                icon: const Icon(Icons.bluetooth_searching),
                label: const Text('Pick'),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Align(
            alignment: Alignment.centerLeft,
            child: OutlinedButton.icon(
              onPressed: () =>
                  BluetoothClassic.instance.openBluetoothSettings(),
              icon: const Icon(Icons.settings_bluetooth),
              label: const Text('Pair radio in Bluetooth settings'),
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _channel,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'Memory channel',
              helperText: 'Blank uses the radio current channel',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _statusPoll,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(labelText: 'Status poll (ms)'),
          ),
          const SizedBox(height: 18),
          _section('BenshiNode link'),
          TextField(
            controller: _linkServer,
            decoration: const InputDecoration(
              labelText: 'Server',
              hintText: 'reflector.benshinode.com:7300',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _linkKey,
            obscureText: true,
            decoration: const InputDecoration(labelText: 'Shared key'),
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Connect on startup'),
            value: _linkAuto,
            onChanged: (value) => setState(() => _linkAuto = value),
          ),
          const SizedBox(height: 24),
          FilledButton.icon(
            onPressed: _saving ? null : _saveAndRestart,
            icon: _saving
                ? const SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.save),
            label: const Text('Save and restart node'),
          ),
          const SizedBox(height: 32),
        ],
      ),
    );
  }

  Widget _section(String title) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Text(
        title,
        style: const TextStyle(
          fontSize: 12,
          fontWeight: FontWeight.w700,
          letterSpacing: 0.8,
          color: Colors.white54,
        ),
      ),
    );
  }
}
