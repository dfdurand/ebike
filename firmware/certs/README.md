# Device credentials

This directory holds the AWS IoT credentials of **your** device. Nothing here
is versioned except this file.

`CONFIG_EBIKE_PROVISION_CERTS` embeds these PEM files in the firmware image at
build time and writes them into the modem at boot, under
`CONFIG_MQTT_HELPER_SEC_TAG`. This is a development convenience: the
credentials are then readable in the flash image. Disable the option and
provision out of band for anything beyond a prototype.

Three files are required:

| File | What it is |
|---|---|
| `AmazonRootCA1.pem` | Amazon root CA, <https://www.amazontrust.com/repository/AmazonRootCA1.pem> |
| `device-cert.pem` | Certificate of your AWS IoT thing |
| `private.key` | Private key of that certificate |

Create them with the AWS CLI:

```bash
aws iot create-keys-and-certificate \
    --set-as-active \
    --certificate-pem-outfile device-cert.pem \
    --private-key-outfile private.key \
    --public-key-outfile public.key
curl -o AmazonRootCA1.pem https://www.amazontrust.com/repository/AmazonRootCA1.pem
chmod 600 private.key
```

Then attach the certificate to your thing and to a policy allowing connect,
publish and subscribe on `bike/<thing-name>/*` and on the shadow topics. See
the README for the full setup.
