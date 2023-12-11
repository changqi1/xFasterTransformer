"""
Convert huggingface ChatGLM model. Use https://huggingface.co/Qwen
"""

import argparse
import configparser
import multiprocessing
import numpy as np
import os
import sys
import torch

from datetime import datetime
from pathlib import Path
from tqdm import tqdm
from transformers import AutoModelForCausalLM, AutoTokenizer

dir_path = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(dir_path, "../../../.."))
sys.path.append(dir_path)


def get_weight_data_type(data_type):
    if data_type == "fp32":
        return np.float32
    elif data_type == "fp16":
        return np.float16
    else:
        assert False, f"Invalid weight data type {data_type}"


def split_and_convert_process(
    i, saved_dir, factor, key, args, val, old_name, dtype, num_attention_heads, num_key_value_heads
):
    def save_val(val, key, tp_num=None):
        if key.startswith("model."):
            path = os.path.join(saved_dir, key)
        else:
            path = os.path.join(saved_dir, "model." + key)

        if tp_num is not None:
            path += "." + str(tp_num)
        path += ".bin"

        val.tofile(path)

    if (
        "input_layernorm.weight" in key
        or "input_layernorm.bias" in key
        or "attention.dense.bias" in key
        or "post_attention_layernorm.weight" in key
        or "post_attention_layernorm.bias" in key
        or "mlp.dense_4h_to_h.bias" in key
        or "final_layernorm.weight" in key
        or "final_layernorm.bias" in key
    ):
        # shared weights, only need to convert the weights of rank 0
        if i == 0:
            save_val(val, key)

    elif "mlp.gate_proj.weight" in key or "mlp.up_proj.weight" in key or "mlp.down_proj.weight" in key:
        split_vals = np.split(val, factor, axis=0)
        for j in range(factor):
            save_val(split_vals[j], key, i * factor + j)

    elif "attention.query_key_value.weight" in key:
        qkvcols = val.shape[-1]
        head_size = int(qkvcols / (int(num_attention_heads) + int(num_key_value_heads) * 2))
        qcol = int(num_attention_heads) * head_size
        kcol = int(num_key_value_heads) * head_size
        vcol = int(num_key_value_heads) * head_size
        qkv = np.split(val, [qcol, (qcol + kcol)], axis=-1)
        q = qkv[0]
        k = qkv[1]
        v = qkv[2]
        q_split_vals = np.split(q, factor, axis=-1)
        k_split_vals = np.split(k, factor, axis=-1)
        v_split_vals = np.split(v, factor, axis=-1)
        for j in range(factor):
            val = np.concatenate((q_split_vals[j], k_split_vals[j], v_split_vals[j]), axis=-1)
            save_val(val, key, i * factor + j)

    elif "attention.query_key_value.bias" in key:
        hidden_dim = val.shape[0]
        head_size = int(hidden_dim / (int(num_attention_heads) + int(num_key_value_heads) * 2))
        qcol = int(num_attention_heads) * head_size
        kcol = int(num_key_value_heads) * head_size
        vcol = int(num_key_value_heads) * head_size
        qkv = np.split(val, [qcol, qcol + kcol])
        q = qkv[0]
        k = qkv[1]
        v = qkv[2]
        q_split_vals = np.split(q, factor, axis=-1)
        k_split_vals = np.split(k, factor, axis=-1)
        v_split_vals = np.split(v, factor, axis=-1)
        for j in range(factor):
            val = np.concatenate((q_split_vals[j], k_split_vals[j], v_split_vals[j]), axis=-1)
            save_val(val, key, i * factor + j)

    elif "attention.dense.weight" in key:
        split_vals = np.split(val, factor, axis=0)
        for j in range(factor):
            save_val(split_vals[j], key, i * factor + j)

    else:
        print("[ERROR] cannot find key '{}'".format(key))


def split_and_convert(args):
    saved_dir = args.saved_dir

    # create directory if not exist
    if not os.path.exists(saved_dir):
        os.makedirs(saved_dir)

    # load the model
    model = AutoModelForCausalLM.from_pretrained(args.in_file, device_map="auto", trust_remote_code=True)

    hf_config = vars(model.config)

    layer_names = [name for name, param in model.named_parameters()]

    # save parameters to config file
    config = configparser.ConfigParser()
    config["qwen"] = {}
    has_post_decoder_layernorm = True
    try:
        config["qwen"]["model_name"] = "qwen" if hf_config["_name_or_path"] == "" else hf_config["_name_or_path"]
        num_attention_heads = config["qwen"]["head_num"] = str(hf_config["num_attention_heads"])
        num_key_value_heads = config["qwen"]["kv_head_num"] = str(
            hf_config.get("num_key_value_heads", num_attention_heads)
        )

        hidden_size = hf_config["hidden_size"]
        config["qwen"]["size_per_head"] = str(hidden_size // hf_config["num_attention_heads"])
        config["qwen"]["inter_size"] = str(hf_config["intermediate_size"])
        config["qwen"]["max_pos_seq_len"] = str(hf_config["max_position_embeddings"])
        config["qwen"]["num_layer"] = str(hf_config["num_hidden_layers"])
        config["qwen"]["rms_norm_eps"] = "1e-6"
        config["qwen"]["layernorm_type"] = "pre_layernorm"
        config["qwen"]["activation_type"] = "silu"
        config["qwen"]["has_post_decoder_layernorm"] = "1" if has_post_decoder_layernorm else "0"
        config["qwen"]["vocab_size"] = str(hf_config["vocab_size"])
        config["qwen"]["start_id"] = str(hf_config["bos_token_id"])
        config["qwen"]["end_id"] = str(hf_config["eos_token_id"])
        config["qwen"]["weight_data_type"] = args.weight_data_type
        with open(os.path.join(saved_dir, "config.ini"), "w") as configfile:
            config.write(configfile)
    except Exception as e:
        print("Fail to save the config in config.ini.", str(e))

    np_weight_data_type = get_weight_data_type(args.weight_data_type)

    hf_model_name_pattern = [
        "ln_1.weight",
        "attn.c_attn.weight",
        "attn.c_attn.bias",
        "attn.c_proj.weight",
        "ln_2.weight",
        "mlp.w2.weight",
        "mlp.w1.weight",
        "mlp.c_proj.weight",
    ]

    ft_model_name_pattern = [
        "input_layernorm.weight",
        "attention.query_key_value.weight",
        "attention.query_key_value.bias",
        "attention.dense.weight",
        "post_attention_layernorm.weight",
        "mlp.gate_proj.weight",
        "mlp.up_proj.weight",
        "mlp.down_proj.weight",
    ]

    state_dict = model.state_dict()
    model_named_parameters = dict()
    for name, param in state_dict.items():
        # merge QKV
        if "self_attn.q_proj.weight" in name:
            k_name = name.replace("q_proj", "k_proj")
            v_name = name.replace("q_proj", "v_proj")
            qkv = torch.cat(
                (param.permute(1, 0), state_dict[k_name].permute(1, 0), state_dict[v_name].permute(1, 0)), dim=1
            )
            model_named_parameters[name.replace("self_attn.q_proj.weight", "attention.query_key_value.weight")] = qkv
        # for merged weights, skip
        elif "self_attn.k_proj.weight" in name or "self_attn.v_proj.weight" in name:
            continue
        elif "embed" in name:
            model_named_parameters[name] = param
        elif "lm_head" in name:
            model_named_parameters[name] = param
        else:
            model_named_parameters[name] = param.permute(1, 0) if len(param.shape) == 2 else param

    print("Processing ...")
    pool = multiprocessing.Pool(args.processes)
    for name, param in model_named_parameters.items():
        if name == "transformer.wte.weight":
            param.detach().cpu().numpy().astype(np_weight_data_type).tofile(os.path.join(saved_dir, "model.wte.bin"))
        elif name == "transformer.ln_f.weight":
            param.detach().cpu().numpy().astype(np_weight_data_type).tofile(
                os.path.join(saved_dir, "model.final_layernorm.weight.bin")
            )
        elif name == "lm_head.weight":
            param.detach().cpu().numpy().astype(np_weight_data_type).tofile(
                os.path.join(saved_dir, "model.lm_head.weight.bin")
            )
        else:
            starmap_args = []
            for i in range(len(hf_model_name_pattern)):
                if hf_model_name_pattern[i] in name:
                    factor = 1
                    new_name = name.replace("transformer.h", "model.layers")
                    new_name = new_name.replace(hf_model_name_pattern[i], ft_model_name_pattern[i])
                    starmap_args.append(
                        (
                            0,
                            saved_dir,
                            factor,
                            new_name,
                            args,
                            param.detach().cpu().numpy().astype(np_weight_data_type),
                            name,
                            np_weight_data_type,
                            num_attention_heads,
                            num_key_value_heads,
                        )
                    )
            pool.starmap_async(split_and_convert_process, starmap_args)
    pool.close()
    pool.join()


if __name__ == "__main__":
    torch.multiprocessing.set_start_method("spawn")
    torch.multiprocessing.set_sharing_strategy("file_system")

    parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--saved_dir", "-o", type=str, help="file name of output file", required=True)
    parser.add_argument("--in_file", "-i", type=str, help="file name of input checkpoint file", required=True)
    parser.add_argument("--processes", "-p", type=int, help="processes to spawn for conversion (default: 8)", default=8)
    parser.add_argument("--weight_data_type", "-d", type=str, default="fp16", choices=["fp32", "fp16"])

    args = parser.parse_args()
    print("\n=============== Argument ===============")
    for key in vars(args):
        print(f"{key}: {vars(args)[key]}")
    print("========================================")

    start_time = datetime.now()
    split_and_convert(args)
    stop_time = datetime.now()
    run_time = stop_time - start_time
    print(f"[INFO] Spend {run_time} (h:m:s) to convert the model")



